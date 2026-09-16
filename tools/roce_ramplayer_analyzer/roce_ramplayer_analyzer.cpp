/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * RoCEv2 receiver + analyzer for the FPGA RAM player
 * (fpga/nv_hsb_ip/data_gen/ram_player.sv, APB base 0x5000_0000 in the
 * pynq/rfsoc-pynq design, psel[4] -> address nibble 0x5).
 *
 * The program:
 *   1. Enumerates the HSB board (bootp) and resolves the data-plane
 *      register addresses (hif/vp) from the enumeration metadata.
 *   2. Sets up an ibverbs UC queue pair with a page-structured receive
 *      buffer, mirroring src/hololink/operators/roce_receiver.
 *   3. Programs the FPGA data-plane registers (VP/HIF map from
 *      src/hololink/core/data_channel.hpp) so the FPGA streams RDMA
 *      writes into our buffer, one write-with-immediate per frame.
 *   4. Polls completions, parses the 128-byte FrameMetadata block the
 *      FPGA appends after each frame (big-endian, layout from
 *      src/hololink/core/hololink.cpp::deserialize_metadata), and
 *      reports statistics: frame rate, throughput, frame-number gaps
 *      (drops), PSN cross-check, size errors, inter-frame jitter and
 *      NIC-level rx_write_requests.
 *
 * Use together with ramplayer_on.sh / ramplayer_off.sh, which poke the
 * RAM-player registers through hololink-write.
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <net/if.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

#include <hololink/core/data_channel.hpp>
#include <hololink/core/enumerator.hpp>
#include <hololink/core/hololink.hpp>
#include <hololink/core/logging.hpp>
#include <hololink/core/networking.hpp>
#include <hololink/core/timeout.hpp>

namespace {

// ---------------------------------------------------------------------------
// FPGA register map (src/hololink/core/data_channel.hpp)
// ---------------------------------------------------------------------------
// HIF: packet metadata
constexpr uint32_t DP_PACKET_SIZE = 0x04; // in 128B pages
constexpr uint32_t DP_PACKET_UDP_PORT = 0x08; // source UDP port the FPGA sends from
constexpr uint32_t DP_VP_MASK = 0x0C; // bit per virtual port: 1 = transmit enabled
// VP: DMA descriptor registers
constexpr uint32_t DP_QP = 0x00; // destination QP number
constexpr uint32_t DP_RKEY = 0x04; // rkey of our MR
constexpr uint32_t DP_PAGE_LSB = 0x08; // VA of page 0
constexpr uint32_t DP_PAGE_MSB = 0x0C;
constexpr uint32_t DP_PAGE_INC = 0x10; // page stride in 128B pages
constexpr uint32_t DP_MAX_BUFF = 0x14; // last page index
constexpr uint32_t DP_BUFFER_LENGTH = 0x18; // bytes per frame (frame-end trigger)
constexpr uint32_t DP_HOST_MAC_LOW = 0x20;
constexpr uint32_t DP_HOST_MAC_HIGH = 0x24;
constexpr uint32_t DP_HOST_IP = 0x28;
constexpr uint32_t DP_HOST_UDP_PORT = 0x2C;

constexpr uint32_t ROCE_OVERHEAD = 74; // bytes of Eth/IP/UDP/BTH/RETH/ICRC per packet
constexpr uint16_t DATA_SOURCE_UDP_PORT = 12288; // hololink.hpp
constexpr uint32_t ROCE_V2_UDP_PORT = 4791; // host receive port we advertise
constexpr uint32_t PAGE_SHIFT = 7; // FPGA "pages" are 128 bytes

inline uint32_t PAGES(uint64_t x) { return static_cast<uint32_t>(x >> PAGE_SHIFT); }

// ---------------------------------------------------------------------------
// FrameMetadata (hololink.cpp::deserialize_metadata) — big endian, 128 bytes
// ---------------------------------------------------------------------------
struct FrameMetadata {
    uint32_t flags;
    uint32_t psn;
    uint32_t crc;
    uint64_t timestamp_s;
    uint32_t timestamp_ns;
    uint64_t bytes_written;
    uint16_t frame_number;
    uint64_t metadata_s;
    uint32_t metadata_ns;
};

class BeReader {
public:
    BeReader(const uint8_t* p, size_t n)
        : p_(p)
        , n_(n)
    {
    }
    uint16_t u16()
    {
        uint16_t v = (static_cast<uint16_t>(p_[0]) << 8) | p_[1];
        p_ += 2;
        n_ -= 2;
        return v;
    }
    uint32_t u32()
    {
        uint32_t v = (static_cast<uint32_t>(p_[0]) << 24) | (static_cast<uint32_t>(p_[1]) << 16)
            | (static_cast<uint32_t>(p_[2]) << 8) | p_[3];
        p_ += 4;
        n_ -= 4;
        return v;
    }
    uint64_t u64()
    {
        uint64_t hi = u32();
        uint64_t lo = u32();
        return (hi << 32) | lo;
    }

private:
    const uint8_t* p_;
    size_t n_;
};

FrameMetadata parse_metadata(const uint8_t* p)
{
    FrameMetadata m {};
    BeReader r(p, hololink::METADATA_SIZE);
    m.flags = r.u32();
    m.psn = r.u32();
    m.crc = r.u32();
    m.timestamp_s = r.u64();
    m.timestamp_ns = r.u32();
    m.bytes_written = r.u64();
    (void)r.u16(); // ignored
    m.frame_number = r.u16();
    m.metadata_s = r.u64();
    m.metadata_ns = r.u32();
    return m;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    std::string hololink_ip = "192.168.0.2";
    std::string ibdev; // empty = auto-detect
    unsigned ibport = 1;
    uint32_t frame_size = 4096; // must equal the RAM player's window_size
    unsigned pages = 64;
    uint64_t max_frames = 0; // 0 = unlimited
    double stats_interval = 2.0; // seconds
    bool configure_fpga = true; // program the VP/HIF registers
    bool verify_pattern = false; // check payload dwords i == expected[i]
    bool dump_first = false; // hex dump of first frame's head/metadata
    bool verbose = false;
};

void print_usage(const char* prog)
{
    std::printf(
        "Usage: %s [OPTIONS]\n"
        "  -H, --hololink=IP      HSB board IP (default 192.168.0.2)\n"
        "  -d, --ibdev=NAME       ibverbs device (default: auto-detect from route)\n"
        "  -p, --ibport=N         ibverbs port (default 1)\n"
        "  -f, --frame-size=N     bytes per frame = RAM player window_size (default 4096)\n"
        "      --pages=N          receive ring pages (default 64, max 4096)\n"
        "  -n, --frames=N         stop after N frames (default: until Ctrl-C)\n"
        "  -i, --interval=SEC     stats print interval (default 2.0)\n"
        "      --no-config        do not program FPGA data-plane registers\n"
        "      --verify-pattern   verify payload against incrementing-dword pattern\n"
        "      --dump-first       hex dump head of first frame + its metadata\n"
        "  -v, --verbose          per-frame log lines\n"
        "  -h, --help\n",
        prog);
}

Options parse_args(int argc, char* argv[])
{
    Options o;
    static struct option long_options[] = {
        { "hololink", required_argument, nullptr, 'H' },
        { "ibdev", required_argument, nullptr, 'd' },
        { "ibport", required_argument, nullptr, 'p' },
        { "frame-size", required_argument, nullptr, 'f' },
        { "pages", required_argument, nullptr, 1000 },
        { "frames", required_argument, nullptr, 'n' },
        { "interval", required_argument, nullptr, 'i' },
        { "no-config", no_argument, nullptr, 1001 },
        { "verify-pattern", no_argument, nullptr, 1002 },
        { "dump-first", no_argument, nullptr, 1003 },
        { "verbose", no_argument, nullptr, 'v' },
        { "help", no_argument, nullptr, 'h' },
        { nullptr, 0, nullptr, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "H:d:p:f:n:i:vh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 'H': o.hololink_ip = optarg; break;
        case 'd': o.ibdev = optarg; break;
        case 'p': o.ibport = std::stoul(optarg); break;
        case 'f': o.frame_size = std::stoul(optarg, nullptr, 0); break;
        case 1000: o.pages = std::stoul(optarg); break;
        case 'n': o.max_frames = std::stoull(optarg); break;
        case 'i': o.stats_interval = std::stod(optarg); break;
        case 1001: o.configure_fpga = false; break;
        case 1002: o.verify_pattern = true; break;
        case 1003: o.dump_first = true; break;
        case 'v': o.verbose = true; break;
        case 'h': print_usage(argv[0]); std::exit(0);
        default: print_usage(argv[0]); std::exit(1);
        }
    }
    if (o.frame_size == 0 || (o.frame_size & 3)) {
        throw std::runtime_error("frame-size must be a non-zero multiple of 4");
    }
    if (o.pages < 1 || o.pages > 4096) {
        throw std::runtime_error("pages must be in [1, 4096]");
    }
    return o;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
struct Stats {
    uint64_t frames = 0; // completed frames (write-with-imm completions)
    uint64_t bytes = 0; // sum of metadata.bytes_written
    uint64_t dropped_frames = 0; // inferred from frame_number gaps
    uint64_t psn_mismatch = 0; // metadata psn != imm psn
    uint64_t size_errors = 0; // bytes_written != frame_size
    uint64_t pattern_errors = 0; // mismatching payload dwords
    uint64_t wc_errors = 0; // non-success completions
    uint64_t other_opcode = 0; // completions that are not recv-rdma-with-imm
    bool have_last_frame_number = false;
    uint16_t last_frame_number = 0;
    // inter-frame interval from the FPGA PTP timestamp (nanoseconds)
    bool have_last_ts = false;
    double last_ts_ns = 0;
    double interval_sum = 0, interval_min = 1e30, interval_max = 0, interval_sq = 0;
    uint64_t interval_count = 0;
    std::map<uint32_t, uint64_t> flag_values;

    void add_interval(double dt_ns)
    {
        interval_sum += dt_ns;
        interval_sq += dt_ns * dt_ns;
        interval_min = std::min(interval_min, dt_ns);
        interval_max = std::max(interval_max, dt_ns);
        interval_count++;
    }

    void print(const char* tag, double wall_s, uint64_t rx_write_requests) const
    {
        const double fps = wall_s > 0 ? frames / wall_s : 0;
        const double mbps = wall_s > 0 ? (bytes * 8.0) / wall_s / 1e6 : 0;
        std::printf("[%s] frames=%llu dropped=%llu psn_mismatch=%llu size_err=%llu"
                    " pattern_err=%llu wc_err=%llu other_op=%llu | %.1f fps, %.1f Mb/s",
            tag,
            (unsigned long long)frames, (unsigned long long)dropped_frames,
            (unsigned long long)psn_mismatch, (unsigned long long)size_errors,
            (unsigned long long)pattern_errors, (unsigned long long)wc_errors,
            (unsigned long long)other_opcode, fps, mbps);
        if (interval_count) {
            const double mean = interval_sum / interval_count;
            const double var = interval_sq / interval_count - mean * mean;
            std::printf(" | frame interval mean %.3f ms min %.3f max %.3f jitter %.3f ms",
                mean / 1e6, interval_min / 1e6, interval_max / 1e6,
                std::sqrt(std::max(0.0, var)) / 1e6);
        }
        if (rx_write_requests != ~0ULL) {
            std::printf(" | nic rx_write_requests=%llu", (unsigned long long)rx_write_requests);
        }
        std::printf("\n");
        if (!flag_values.empty()) {
            std::printf("  flags:");
            for (const auto& [v, n] : flag_values) {
                std::printf(" %#x=%llu", v, (unsigned long long)n);
            }
            std::printf("\n");
        }
    }
};

uint64_t read_rx_write_requests(const char* ibdev, unsigned port)
{
    char path[256];
    std::snprintf(path, sizeof(path),
        "/sys/class/infiniband/%s/ports/%u/hw_counters/rx_write_requests", ibdev, port);
    FILE* f = std::fopen(path, "r");
    if (!f) {
        return ~0ULL;
    }
    uint64_t v = ~0ULL;
    if (std::fscanf(f, "%llu", (unsigned long long*)&v) != 1) {
        v = ~0ULL;
    }
    std::fclose(f);
    return v;
}

// ---------------------------------------------------------------------------
// ibverbs helpers
// ---------------------------------------------------------------------------

// Find (device, gid_index) whose RoCEv2 GID embeds local_ip (::FFFF:a.b.c.d),
// mirroring RoceReceiver::start()'s GID scan. If ibdev_name is non-empty,
// restrict the search to that device.
std::tuple<std::string, int> find_roce_v2_gid(const std::string& local_ip,
    const std::string& ibdev_name, unsigned ibport)
{
    in_addr_t ip_nbo = 0;
    if (inet_pton(AF_INET, local_ip.c_str(), &ip_nbo) != 1) {
        throw std::runtime_error("invalid local ip " + local_ip);
    }
    const uint64_t want_interface_id = (static_cast<uint64_t>(ip_nbo) << 32) | 0xFFFF0000;

    int num_devices = 0;
    ibv_device** devices = ibv_get_device_list(&num_devices);
    if (!devices || num_devices <= 0) {
        throw std::runtime_error("no ibverbs devices found");
    }
    for (int i = 0; i < num_devices; i++) {
        const char* name = ibv_get_device_name(devices[i]);
        if (!ibdev_name.empty() && ibdev_name != name) {
            continue;
        }
        ibv_context* ctx = ibv_open_device(devices[i]);
        if (!ctx) {
            continue;
        }
        for (int gid_index = 0;; gid_index++) {
            ibv_gid_entry entry {};
            int r = ibv_query_gid_ex(ctx, ibport, gid_index, &entry, 0);
            if (r && errno == ENODATA) {
                break; // end of table
            }
            if (r) {
                break;
            }
            if (entry.gid_type != IBV_GID_TYPE_ROCE_V2) {
                continue;
            }
            if (entry.gid.global.subnet_prefix != 0) {
                continue;
            }
            if (entry.gid.global.interface_id != want_interface_id) {
                continue;
            }
            std::string found = name;
            ibv_close_device(ctx);
            ibv_free_device_list(devices);
            return { found, gid_index };
        }
        ibv_close_device(ctx);
    }
    ibv_free_device_list(devices);
    throw std::runtime_error("no RoCEv2 GID matching local ip " + local_ip
        + (ibdev_name.empty() ? "" : (" on device " + ibdev_name)));
}

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

double now_s()
{
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char* argv[])
{
    hololink::logging::hsb_log_level = hololink::logging::HSB_LOG_LEVEL_WARN;
    const Options opt = parse_args(argc, argv);

    // ------------------------------------------------------------------
    // 1. Enumerate the board, resolve data-plane register addresses.
    //    The pynq control plane sporadically drops transactions (worse
    //    while the RAM player is streaming); retry the whole control-plane
    //    setup a few times before giving up.
    // ------------------------------------------------------------------
    hololink::Metadata metadata;
    std::shared_ptr<hololink::Hololink> hololink;
    uint32_t hif_address = 0, vp_address = 0, vp_mask = 0;
    int64_t data_plane = 0, sensor = 0;
    for (int attempt = 1;; attempt++) {
        try {
            metadata = hololink::Enumerator::find_channel(opt.hololink_ip);
            // Skip the PTP register configuration inside Hololink::start(): on
            // the pynq design those writes sporadically time out, and the
            // analysis here only needs the (free-running) FPGA timestamp
            // deltas anyway.
            metadata["ptp_enable"] = static_cast<int64_t>(0);
            // Disable control-plane sequence-number checking: concurrent
            // hololink-write invocations (ramplayer_on/off.sh) bump the
            // board's sequence number, which would otherwise fail this
            // session's read-modify-write of DP_VP_MASK with
            // RESPONSE_SEQUENCE_CHECK_FAIL.
            metadata["sequence_number_checking"] = static_cast<int64_t>(0);
            hololink::DataChannel channel(metadata);
            hololink = channel.hololink();
            auto need = [&](const char* key) {
                auto v = metadata.get<int64_t>(key);
                if (!v) {
                    throw std::runtime_error(std::string("enumeration metadata lacks '") + key + "'");
                }
                return v.value();
            };
            hif_address = static_cast<uint32_t>(need("hif_address"));
            vp_address = static_cast<uint32_t>(need("vp_address"));
            vp_mask = static_cast<uint32_t>(need("vp_mask"));
            data_plane = need("data_plane");
            sensor = need("sensor");
            break;
        } catch (const std::exception& e) {
            if (attempt >= 5) {
                throw;
            }
            std::fprintf(stderr, "control-plane setup attempt %d failed (%s); retrying\n",
                attempt, e.what());
            usleep(500 * 1000);
        }
    }

    std::printf("board: peer=%s data_plane=%lld sensor=%lld hif=%#x vp=%#x vp_mask=%#x\n",
        opt.hololink_ip.c_str(), (long long)data_plane, (long long)sensor,
        hif_address, vp_address, vp_mask);

    auto [local_ip, local_device, local_mac] = hololink::core::local_ip_and_mac(opt.hololink_ip);
    std::printf("host: ip=%s dev=%s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
        local_ip.c_str(), local_device.c_str(),
        local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5]);

    // ------------------------------------------------------------------
    // 2. Receive buffer: ring of `pages` pages; page = frame + 128B metadata.
    // ------------------------------------------------------------------
    const size_t aligned_frame = hololink::core::round_up(opt.frame_size, hololink::core::PAGE_SIZE);
    const size_t metadata_offset = aligned_frame;
    const size_t page_size = aligned_frame + hololink::METADATA_SIZE;
    const size_t buffer_size = page_size * opt.pages;

    void* buffer = nullptr;
    if (posix_memalign(&buffer, 4096, buffer_size)) {
        throw std::runtime_error("posix_memalign failed");
    }
    std::memset(buffer, 0xEE, buffer_size);
    std::printf("buffer: %zu bytes = %u pages x %zu (frame %#x + metadata 0x80)\n",
        buffer_size, opt.pages, page_size, opt.frame_size);

    // ------------------------------------------------------------------
    // 3. ibverbs: UC QP, mirroring RoceReceiver::start().
    // ------------------------------------------------------------------
    auto [ibdev, gid_index] = find_roce_v2_gid(local_ip, opt.ibdev, opt.ibport);
    std::printf("roce: device=%s port=%u gid_index=%d\n", ibdev.c_str(), opt.ibport, gid_index);

    ibv_device** dev_list = ibv_get_device_list(nullptr);
    ibv_context* ctx = nullptr;
    for (int i = 0; dev_list[i]; i++) {
        if (ibdev == ibv_get_device_name(dev_list[i])) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }
    ibv_free_device_list(dev_list);
    if (!ctx) {
        throw std::runtime_error("ibv_open_device failed for " + ibdev);
    }
    ibv_pd* pd = ibv_alloc_pd(ctx);
    ibv_comp_channel* comp_channel = ibv_create_comp_channel(ctx);
    ibv_cq* cq = ibv_create_cq(ctx, 256, nullptr, comp_channel, 0);
    ibv_mr* mr = ibv_reg_mr(pd, buffer, buffer_size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!pd || !comp_channel || !cq || !mr) {
        throw std::runtime_error("ibverbs setup (pd/cq/mr) failed");
    }

    ibv_qp_init_attr qp_init {};
    qp_init.send_cq = cq;
    qp_init.recv_cq = cq;
    qp_init.cap.max_send_wr = 64;
    qp_init.cap.max_recv_wr = 2048;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;
    qp_init.qp_type = IBV_QPT_UC; // HSB streams via Unreliable Connected
    ibv_qp* qp = ibv_create_qp(pd, &qp_init);
    if (!qp) {
        throw std::runtime_error("ibv_create_qp failed");
    }
    const uint32_t qpn = qp->qp_num;
    const uint32_t rkey = mr->rkey;

    ibv_qp_attr attr {};
    attr.qp_state = IBV_QPS_INIT;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    attr.pkey_index = 0;
    attr.port_num = static_cast<uint8_t>(opt.ibport);
    if (ibv_modify_qp(qp, &attr,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        throw std::runtime_error("modify_qp INIT failed");
    }

    // recv WRs: each write-with-imm completion consumes one; no sge needed.
    // Post up to the queue depth and replenish one per received frame below.
    uint64_t next_wr_id = 0;
    auto post_one_recv = [&]() {
        ibv_recv_wr wr {};
        wr.wr_id = next_wr_id++;
        ibv_recv_wr* bad = nullptr;
        if (ibv_post_recv(qp, &wr, &bad)) {
            throw std::runtime_error("ibv_post_recv failed");
        }
    };
    for (unsigned i = 0; i < 2048; i++) {
        post_one_recv();
    }

    in_addr_t peer_nbo = 0;
    if (inet_pton(AF_INET, opt.hololink_ip.c_str(), &peer_nbo) != 1) {
        throw std::runtime_error("invalid peer ip");
    }
    union ibv_gid dgid {};
    dgid.global.subnet_prefix = 0;
    dgid.global.interface_id = (static_cast<uint64_t>(peer_nbo) << 32) | 0xFFFF0000;
    attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.rq_psn = 0;
    attr.dest_qp_num = 0;
    attr.ah_attr.grh.dgid = dgid;
    attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(gid_index);
    attr.ah_attr.grh.hop_limit = 0xFF;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = static_cast<uint8_t>(opt.ibport);
    for (int retry = 5;; retry--) {
        if (!ibv_modify_qp(qp, &attr,
                IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN)) {
            break;
        }
        if (!retry) {
            throw std::runtime_error("modify_qp RTR failed");
        }
        usleep(200 * 1000);
    }
    std::printf("qp: qpn=%u rkey=%#x\n", qpn, rkey);

    // ------------------------------------------------------------------
    // 4. Program the FPGA data plane (DataChannel::configure_roce mirror).
    // ------------------------------------------------------------------
    // payload per packet: largest 128B multiple within MTU budget.
    const uint32_t payload_size
        = ((hololink::core::DEFAULT_MTU - ROCE_OVERHEAD) / hololink::core::PAGE_SIZE)
        * hololink::core::PAGE_SIZE;

    const uint32_t mac_high = (local_mac[0] << 8) | local_mac[1];
    const uint32_t mac_low = (local_mac[2] << 24) | (local_mac[3] << 16)
        | (local_mac[4] << 8) | local_mac[5];
    const uint32_t host_ip = inet_network(local_ip.c_str());
    const uint64_t va = reinterpret_cast<uint64_t>(buffer);

    // start() + data-plane programming, with the same retry policy as the
    // enumeration above (the pynq control plane drops transactions in
    // bursts, especially while the RAM player is streaming).
    for (int attempt = 1;; attempt++) {
        try {
            // The default 0.5 s per-transaction timeout is too tight on this
            // design; retry each write for up to 10 s (the same policy the
            // stack uses for slow ARP transactions).
            auto t = std::make_shared<hololink::Timeout>(10.f, 0.2f);

            hololink->start();
            if (opt.configure_fpga) {
                // stop transmission while updating
                hololink->write_uint32(hif_address + DP_VP_MASK,
                    hololink->read_uint32(hif_address + DP_VP_MASK, t) & ~vp_mask, t);
                hololink->write_uint32(hif_address + DP_PACKET_SIZE, PAGES(payload_size), t);
                hololink->write_uint32(hif_address + DP_PACKET_UDP_PORT, DATA_SOURCE_UDP_PORT, t);
                hololink->write_uint32(vp_address + DP_BUFFER_LENGTH, opt.frame_size, t);
                hololink->write_uint32(vp_address + DP_HOST_MAC_LOW, mac_low, t);
                hololink->write_uint32(vp_address + DP_HOST_MAC_HIGH, mac_high, t);
                hololink->write_uint32(vp_address + DP_HOST_IP, host_ip, t);
                hololink->write_uint32(vp_address + DP_HOST_UDP_PORT, ROCE_V2_UDP_PORT, t);
                hololink->write_uint32(vp_address + DP_QP, qpn, t);
                hololink->write_uint32(vp_address + DP_RKEY, rkey, t);
                hololink->write_uint32(vp_address + DP_PAGE_LSB, static_cast<uint32_t>(va), t);
                hololink->write_uint32(vp_address + DP_PAGE_MSB, static_cast<uint32_t>(va >> 32), t);
                hololink->write_uint32(vp_address + DP_PAGE_INC, PAGES(page_size), t);
                hololink->write_uint32(vp_address + DP_MAX_BUFF, opt.pages - 1, t);
                // enable this virtual port
                hololink->write_uint32(hif_address + DP_VP_MASK,
                    hololink->read_uint32(hif_address + DP_VP_MASK, t) | vp_mask, t);
                std::printf("fpga: data plane configured (payload=%uB/packet, vp_mask=%#x enabled)\n",
                    payload_size, vp_mask);
            }
            break;
        } catch (const std::exception& e) {
            if (attempt >= 5) {
                throw;
            }
            std::fprintf(stderr, "FPGA configure attempt %d failed (%s); retrying\n",
                attempt, e.what());
            usleep(500 * 1000);
        }
    }

    // ------------------------------------------------------------------
    // 5. Completion loop.
    // ------------------------------------------------------------------
    if (opt.max_frames) {
        std::printf("receiving up to %llu frames... (Ctrl-C to stop)\n",
            (unsigned long long)opt.max_frames);
    } else {
        std::printf("receiving... (Ctrl-C to stop)\n");
    }
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    Stats total {}, interval {};
    const uint64_t rxw0 = read_rx_write_requests(ibdev.c_str(), opt.ibport);
    double t0 = now_s(), t_interval = t0;
    if (ibv_req_notify_cq(cq, 0)) {
        throw std::runtime_error("ibv_req_notify_cq failed");
    }

    while (!g_stop) {
        ibv_wc wc {};
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) {
            throw std::runtime_error("ibv_poll_cq failed");
        }
        if (n == 0) {
            usleep(1000);
            if (opt.stats_interval > 0 && now_s() - t_interval >= opt.stats_interval) {
                const double now = now_s();
                interval.print("interval", now - t_interval, ~0ULL);
                interval = Stats {};
                t_interval = now;
            }
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            total.wc_errors++;
            interval.wc_errors++;
            std::fprintf(stderr, "WC error: status=%s vendor=%#x opcode=%d\n",
                ibv_wc_status_str(wc.status), wc.vendor_err, (int)wc.opcode);
            continue;
        }
        if (wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM || !(wc.wc_flags & IBV_WC_WITH_IMM)) {
            total.other_opcode++;
            interval.other_opcode++;
            continue;
        }

        const uint32_t imm = ntohl(wc.imm_data);
        const uint32_t page = imm & 0xFFF; // RoceReceiver::page_from_imm
        const uint32_t imm_psn = (imm >> 12) & 0xFFFFF; // RoceReceiver::psn_from_imm
        post_one_recv(); // replenish the consumed work request
        if (page >= opt.pages) {
            std::fprintf(stderr, "invalid page=%u in imm=%#x; ignored\n", page, imm);
            total.wc_errors++;
            continue;
        }

        const uint8_t* frame = static_cast<const uint8_t*>(buffer) + page * page_size;
        const FrameMetadata m = parse_metadata(frame + metadata_offset);

        Stats* s[2] = { &total, &interval };
        for (Stats* st : s) {
            st->frames++;
            st->bytes += m.bytes_written;
            st->flag_values[m.flags]++;
            if (m.bytes_written != opt.frame_size) {
                st->size_errors++;
            }
            if (st->have_last_frame_number) {
                const uint16_t gap = static_cast<uint16_t>(m.frame_number - st->last_frame_number - 1);
                if (gap && gap < 0x8000) {
                    st->dropped_frames += gap;
                }
            }
            st->last_frame_number = m.frame_number;
            st->have_last_frame_number = true;
            if ((m.psn & 0xFFFFF) != imm_psn) { // RoceReceiver::same_psn
                st->psn_mismatch++;
            }
            const double ts_ns = m.timestamp_s * 1e9 + m.timestamp_ns;
            if (st->have_last_ts && ts_ns > st->last_ts_ns) {
                st->add_interval(ts_ns - st->last_ts_ns);
            }
            st->last_ts_ns = ts_ns;
            st->have_last_ts = true;
        }

        if (opt.verify_pattern) {
            const uint32_t* payload = reinterpret_cast<const uint32_t*>(frame);
            for (uint32_t i = 0; i < opt.frame_size / 4 && total.pattern_errors < 1'000'000; i++) {
                if (payload[i] != i) {
                    if (total.pattern_errors < 5) {
                        std::fprintf(stderr, "pattern mismatch frame=%llu dword=%u: got %#x\n",
                            (unsigned long long)total.frames, i, payload[i]);
                    }
                    total.pattern_errors++;
                    interval.pattern_errors++;
                }
            }
        }

        if (opt.dump_first && total.frames == 1) {
            std::printf("first frame head:");
            for (int i = 0; i < 64; i++) {
                std::printf("%s%02x", (i % 16) ? "" : "\n  ", frame[i]);
            }
            std::printf("\nmetadata: flags=%#x psn=%#x crc=%#x ts=%llu.%09u"
                        " bytes_written=%llu frame_number=%u meta_ts=%llu.%09u\n",
                m.flags, m.psn, m.crc,
                (unsigned long long)m.timestamp_s, m.timestamp_ns,
                (unsigned long long)m.bytes_written, m.frame_number,
                (unsigned long long)m.metadata_s, m.metadata_ns);
        }
        if (opt.verbose) {
            std::printf("frame #%llu: page=%u fn=%u bytes=%llu psn=%#x imm_psn=%#x flags=%#x\n",
                (unsigned long long)total.frames, page, m.frame_number,
                (unsigned long long)m.bytes_written, m.psn, imm_psn, m.flags);
        }
        if (opt.max_frames && total.frames >= opt.max_frames) {
            break;
        }
    }

    const double wall = now_s() - t0;
    const uint64_t rxw1 = read_rx_write_requests(ibdev.c_str(), opt.ibport);

    // ------------------------------------------------------------------
    // 6. Teardown: stop the FPGA data plane, print summary.
    // ------------------------------------------------------------------
    if (opt.configure_fpga) {
        auto t = std::make_shared<hololink::Timeout>(10.f, 0.2f);
        hololink->write_uint32(hif_address + DP_VP_MASK,
            hololink->read_uint32(hif_address + DP_VP_MASK, t) & ~vp_mask, t);
    }
    total.print("TOTAL", wall, (rxw0 != ~0ULL && rxw1 != ~0ULL) ? rxw1 - rxw0 : ~0ULL);

    ibv_destroy_qp(qp);
    ibv_dereg_mr(mr);
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(comp_channel);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    free(buffer);
    return (total.frames > 0) ? 0 : 2;
}
