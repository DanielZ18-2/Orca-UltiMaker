// MakerBot / UltiMaker Fork - Orca Slicer 2.4
// MakerbotDiscoveryDialog.cpp - Network auto-discovery

#include "MakerbotDiscoveryDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/MakerbotLink.hpp"

#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/busyinfo.h>

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <nlohmann/json.hpp>

// UDP includes for port 12307 broadcast
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#include <cstring>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace Slic3r {
namespace GUI {

// -- mDNS Service Names --------------------------------------------------------
// Bonjour builds "_<service>._<protocol>.local" itself, so the names go in
// without the leading underscore -- with it, the query asked for
// "__makerbot._tcp.local" and nothing ever answered. Birdwing printers
// announce "_makerbot-jsonrpc._tcp" (kaiten, firmware 2.6.3), UltiMaker
// printers "_ultimaker._tcp" (what Cura's UM3NetworkPrinting looks for).
static constexpr const char* MDNS_MAKERBOT  = "makerbot-jsonrpc";
static constexpr const char* MDNS_ULTIMAKER = "ultimaker";
static constexpr int         UDP_DISCOVERY_PORT = 12307;  // MakerBot Birdwing beacon
static constexpr int         DISCOVERY_TIMEOUT_MS = 5000;

// -- Constructor ---------------------------------------------------------------

MakerbotDiscoveryDialog::MakerbotDiscoveryDialog(wxWindow* parent, int filter)
    : wxDialog(parent, wxID_ANY,
               filter == 0 ? _L("Discover MakerBot Printers") :
               filter == 1 ? _L("Discover UltiMaker Printers") :
                             _L("Discover MakerBot & UltiMaker Printers"),
               wxDefaultPosition, wxSize(600, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_timer(this)
    , m_filter(filter)
{
    SetFont(wxGetApp().normal_font());

    // -- List control ----------------------------------------------------------
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);
    m_list->InsertColumn(0, _L("Printer Name"),  wxLIST_FORMAT_LEFT, 160);
    m_list->InsertColumn(1, _L("Model"),         wxLIST_FORMAT_LEFT, 120);
    m_list->InsertColumn(2, _L("IP Address"),    wxLIST_FORMAT_LEFT, 120);
    m_list->InsertColumn(3, _L("Protocol"),      wxLIST_FORMAT_LEFT, 170);

    // -- Status line -----------------------------------------------------------
    m_status = new wxStaticText(this, wxID_ANY, _L("Scanning network..."));

    // -- Buttons ---------------------------------------------------------------
    m_rescan = new wxButton(this, wxID_ANY, _L("Rescan"));
    m_ok     = new wxButton(this, wxID_OK,  _L("Select"));
    auto cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    m_ok->Disable();

    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->Add(m_rescan, 0, wxALL, 5);
    btn_sizer->AddStretchSpacer(1);
    btn_sizer->Add(m_ok,     0, wxALL, 5);
    btn_sizer->Add(cancel,   0, wxALL, 5);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_status, 0, wxALL | wxEXPAND, 8);
    main_sizer->Add(m_list,   1, wxALL | wxEXPAND, 8);
    main_sizer->Add(btn_sizer, 0, wxEXPAND);
    SetSizerAndFit(main_sizer);
    SetMinSize(wxSize(500, 320));

    // -- Event bindings --------------------------------------------------------
    m_ok->Bind(wxEVT_BUTTON,     &MakerbotDiscoveryDialog::on_ok,     this);
    m_rescan->Bind(wxEVT_BUTTON, &MakerbotDiscoveryDialog::on_rescan, this);
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { EndModal_if_selected(); });
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED,  [this](auto&) { m_ok->Enable(); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED,[this](auto&) { m_ok->Disable(); });
    Bind(wxEVT_TIMER, &MakerbotDiscoveryDialog::on_timer, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { stop_discovery(); e.Skip(); });

    start_discovery();
}

MakerbotDiscoveryDialog::~MakerbotDiscoveryDialog()
{
    stop_discovery();
}

// -- Discovery -----------------------------------------------------------------

void MakerbotDiscoveryDialog::start_discovery()
{
    m_stop    = false;
    m_results.clear();
    m_list->DeleteAllItems();
    m_ok->Disable();
    m_status->SetLabel(_L("Scanning network for MakerBot and UltiMaker printers..."));

    m_thread = std::thread([this] {
        std::vector<DiscoveredPrinter> found;

        if (m_filter == 0 || m_filter == 2) {
            discover_mdns_makerbot(found);
            discover_udp_broadcast(found);
            if (found.empty())  // Fallback: direkter Subnet-Scan Port 12309
                discover_subnet_scan(found);
        }
        if (m_filter == 1 || m_filter == 2) {
            discover_mdns_ultimaker(found);
        }

        // Deliver results to UI thread
        wxTheApp->CallAfter([this, found] {
            for (const auto& p : found)
                if (!m_stop) add_result(p);
            const int n = m_list->GetItemCount();
            m_status->SetLabel(
                n == 0 ? _L("No printers found. Check network and try Rescan.")
                       : wxString::Format(_L("Found %d printer(s). Select one and click Select."), n));
            m_status->Refresh();
            if (n > 0) m_list->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        });
    });
}

void MakerbotDiscoveryDialog::stop_discovery()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void MakerbotDiscoveryDialog::add_result(const DiscoveredPrinter& p)
{
    // Deduplication by IP
    for (const auto& r : m_results)
        if (r.ip == p.ip) return;
    m_results.push_back(p);

    const long idx = m_list->GetItemCount();
    m_list->InsertItem(idx, from_u8(p.name));
    m_list->SetItem(idx, 1, from_u8(p.model));
    m_list->SetItem(idx, 2, from_u8(p.ip));
    m_list->SetItem(idx, 3, from_u8(p.protocol));
}

// -- mDNS lookup --------------------------------------------------------------
// Bonjour::lookup() runs on a thread of its own and detaches it, so its
// callbacks can still arrive after the wait below has given up. They write
// into state they share with this function, never into its locals. Replies
// over IPv6 are left out: the host field takes the bare address, and a
// printer that answers over both would be listed twice.
static std::vector<BonjourReply> mdns_lookup(const char* service, Bonjour::TxtKeys txt_keys,
                                             const std::atomic<bool>& stop)
{
    struct State
    {
        std::mutex                m;
        std::condition_variable   cv;
        std::vector<BonjourReply> replies;
        bool                      done = false;
    };
    auto state = std::make_shared<State>();
    const Bonjour::Ptr lookup = Bonjour(service)
                                    .set_protocol("tcp")
                                    .set_txt_keys(std::move(txt_keys))
                                    .set_timeout(DISCOVERY_TIMEOUT_MS / 1000)
                                    .set_retries(1)
                                    .on_reply([state](BonjourReply&& reply) {
                                        std::lock_guard<std::mutex> lock(state->m);
                                        if (reply.ip.is_v4())
                                            state->replies.push_back(std::move(reply));
                                    })
                                    .on_complete([state] {
                                        {
                                            std::lock_guard<std::mutex> lock(state->m);
                                            state->done = true;
                                        }
                                        state->cv.notify_all();
                                    })
                                    .lookup();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(DISCOVERY_TIMEOUT_MS + 1000);
    std::unique_lock<std::mutex> lock(state->m);
    while (!state->done && !stop && std::chrono::steady_clock::now() < until)
        state->cv.wait_for(lock, std::chrono::milliseconds(100));
    return state->replies;
}

static std::string txt_value(const BonjourReply& reply, const std::string& key, const std::string& fallback)
{
    const auto it = reply.txt_data.find(key);
    return it == reply.txt_data.end() || it->second.empty() ? fallback : it->second;
}

// -- mDNS Discovery: MakerBot --------------------------------------------------

void MakerbotDiscoveryDialog::discover_mdns_makerbot(std::vector<DiscoveredPrinter>& out)
{
    try {
        for (const BonjourReply& r : mdns_lookup(MDNS_MAKERBOT, {"machine_type", "machine_name"}, m_stop)) {
            if (m_stop) break;
            DiscoveredPrinter p;
            p.ip          = r.ip.to_string();
            p.name        = txt_value(r, "machine_name", r.hostname.empty() ? "MakerBot" : r.hostname);
            p.is_makerbot = true;
            // The service is kaiten's JSON-RPC. As for a printer the UDP
            // beacon finds (discover_udp_broadcast()), only the bare address
            // goes to the printer settings, and MakerbotLink connects to a
            // bare address over SSL on port 12309, a Method as well as a
            // Birdwing printer.
            p.is_birdwing = true;
            p.port        = 12309;
            p.protocol    = "MakerBot Birdwing (mDNS, SSL:12309)";
            p.model       = txt_value(r, "machine_type", "Birdwing");

            BOOST_LOG_TRIVIAL(info) << "MakerbotDiscovery: found " << p.name
                                    << " at " << p.ip << " (" << p.protocol << ")";
            out.push_back(p);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDiscovery mDNS: " << e.what();
    }
}

// -- mDNS Discovery: UltiMaker -------------------------------------------------

void MakerbotDiscoveryDialog::discover_mdns_ultimaker(std::vector<DiscoveredPrinter>& out)
{
    try {
        for (const BonjourReply& r : mdns_lookup(MDNS_ULTIMAKER, {"name"}, m_stop)) {
            if (m_stop) break;
            DiscoveredPrinter p;
            p.ip           = r.ip.to_string();
            p.name         = txt_value(r, "name", r.hostname.empty() ? "UltiMaker" : r.hostname);
            p.is_ultimaker = true;
            p.port         = r.port > 0 ? r.port : 80;
            // The model stays generic: the only machine field in the TXT
            // record, "machine", holds Cura's numeric machine id.
            p.model        = "UltiMaker";
            p.protocol     = "UltiMaker S/Classic/Factor (REST:" + std::to_string(p.port) + ")";

            BOOST_LOG_TRIVIAL(info) << "MakerbotDiscovery: found UltiMaker " << p.name
                                    << " at " << p.ip << " (mDNS)";
            out.push_back(p);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDiscovery mDNS UltiMaker: " << e.what();
    }
}

// -- UDP Broadcast Discovery (MakerBot Birdwing Port 12307) -------------------

void MakerbotDiscoveryDialog::discover_udp_broadcast(std::vector<DiscoveredPrinter>& out)
{
    // MakerBot Birdwing printers listen on UDP port 12307 for discovery packets.
    // Send empty JSON-RPC discovery beacon and collect replies.
    try {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) return;
        const BOOL broadcast = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
        DWORD timeout_ms = 2000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return;
        const int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        struct timeval tv { 2, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        // Send broadcast beacon
        const std::string beacon = R"({"jsonrpc":"2.0","method":"broadcast","id":0,"params":{}})";
        sockaddr_in dest{};
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(UDP_DISCOVERY_PORT);
        dest.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(sock, beacon.c_str(), (int)beacon.size(), 0,
               (struct sockaddr*)&dest, sizeof(dest));

        // Collect replies for 2 seconds
        char buf[4096];
        sockaddr_in from{};
        socklen_t   from_len = sizeof(from);
        while (!m_stop) {
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr*)&from, &from_len);
            if (n <= 0) break;
            buf[n] = '\0';

            DiscoveredPrinter p;
            p.ip          = inet_ntoa(from.sin_addr);
            p.is_makerbot = true;
            p.is_birdwing = true;
            p.port        = 12309;
            p.protocol    = "MakerBot Birdwing (UDP beacon, SSL:12309)";
            p.name        = "MakerBot (UDP)";
            p.model       = "Birdwing";
            // Try to parse name from response
            try {
                auto j = nlohmann::json::parse(std::string(buf, n));
                if (j.contains("result") && j["result"].contains("machine_name"))
                    p.name = j["result"]["machine_name"].get<std::string>();
                if (j.contains("result") && j["result"].contains("machine_type"))
                    p.model = j["result"]["machine_type"].get<std::string>();
            } catch (...) {}

            BOOST_LOG_TRIVIAL(info) << "MakerbotDiscovery UDP: found " << p.name
                                    << " at " << p.ip;
            out.push_back(p);
        }
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerbotDiscovery UDP: " << e.what();
    }
}

// -- Event Handlers ------------------------------------------------------------

void MakerbotDiscoveryDialog::EndModal_if_selected()
{
    wxCommandEvent dummy;
    on_ok(dummy);
}


// -- Subnet Scan: TCP-Connect auf Port 12309 -----------------------------------
// Neither mDNS nor the UDP broadcast crosses subnet boundaries.
// Therefore: parallel TCP scan of all hosts in the /24 subnet.
void MakerbotDiscoveryDialog::discover_subnet_scan(std::vector<DiscoveredPrinter>& out)
{
    // Bekannte private Heimnetz-Subnetz-Prefixes
    // (covers the most common router configurations)
    std::vector<std::string> prefixes = {
        "192.168.2.", "192.168.1.", "192.168.0.", "192.168.3.",
        "192.168.178.", "10.0.0.", "10.0.1."
    };

    // Local subnet prefix via /proc/net/if_inet6 and hostname -I fallback
    {
        // Read local IPs from /proc/net/fib_trie (Linux-specific, reliable)
        std::ifstream fib("/proc/net/fib_trie");
        std::string line;
        std::string last_local;
        while (std::getline(fib, line)) {
            // Search for "LOCAL" entries
            if (line.find("LOCAL") != std::string::npos && !last_local.empty()) {
                const auto last_dot = last_local.rfind('.');
                if (last_dot != std::string::npos) {
                    std::string prefix = last_local.substr(0, last_dot + 1);
                    if (!prefix.empty() && prefix[0] != '0' &&
                        prefix.find("127.") != 0 &&
                        std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end())
                        prefixes.push_back(prefix);
                }
                last_local.clear();
            }
            // Extrahiere IP-Adressen
            const auto start = line.find_first_not_of(" 	|+-");
            if (start != std::string::npos) {
                const std::string trimmed = line.substr(start);
                // Check whether it looks like an IPv4 address
                int a,b,c,d;
                if (sscanf(trimmed.c_str(), "%d.%d.%d.%d", &a,&b,&c,&d) == 4)
                    last_local = std::to_string(a)+"."+std::to_string(b)+"."+
                                 std::to_string(c)+"."+std::to_string(d);
            }
        }
    }

    // For each prefix: check all 254 hosts in parallel
    for (const auto& prefix : prefixes) {
        if (m_stop) break;
        BOOST_LOG_TRIVIAL(info) << "MakerbotDiscovery: scanning " << prefix << "0/24 port 12309";

        std::vector<std::thread> threads;
        std::mutex               results_mutex;
        std::vector<DiscoveredPrinter> found_here;
        std::atomic<int>         pending { 254 };

        for (int i = 1; i <= 254; ++i) {
            if (m_stop) break;
            const std::string ip = prefix + std::to_string(i);

            threads.emplace_back([ip, &found_here, &results_mutex, &pending, this] {
                // Non-blocking TCP connect with 400ms timeout
#ifdef _WIN32
                SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock == INVALID_SOCKET) { --pending; return; }
#else
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) { --pending; return; }
#endif

                // Set non-blocking
#ifdef _WIN32
                u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb);
#else
                fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

                struct sockaddr_in addr {};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(12309);
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

                connect(sock, (struct sockaddr*)&addr, sizeof(addr));

                // Non-blocking connect: completion is signaled
                // in the WRITE fd_set (or EXCEPT fd_set on error).
                fd_set write_fds, err_fds;
                FD_ZERO(&write_fds); FD_ZERO(&err_fds);
                FD_SET(sock, &write_fds);
                FD_SET(sock, &err_fds);
                struct timeval tv { 1, 200000 }; // 1200ms - Z18 needs more time

                if (select(sock + 1, nullptr, &write_fds, &err_fds, &tv) > 0) {
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    if (err == 0 && FD_ISSET(sock, &write_fds) && !m_stop) {
                        // Port 12309 open -> likely Birdwing printer
                        DiscoveredPrinter p;
                        p.ip          = ip;
                        p.is_makerbot = true;
                        p.is_birdwing = true;
                        p.port        = 12309;
                        p.protocol    = "MakerBot Birdwing (SSL:12309)";
                        p.name        = "MakerBot Birdwing";
                        p.model       = "Z18/Replicator+";
                        BOOST_LOG_TRIVIAL(info) << "MakerbotDiscovery: found port 12309 at " << ip;
                        std::lock_guard<std::mutex> lock(results_mutex);
                        found_here.push_back(p);
                    }
                }
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                --pending;
            });
        }

        // Wait until all threads finished or stop
        for (int wait = 0; wait < 100 && pending > 0 && !m_stop; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        for (auto& th : threads)
            if (th.joinable()) th.detach();

        for (const auto& p : found_here)
            out.push_back(p);

        if (!found_here.empty()) break; // found -> stop scanning
    }
}

void MakerbotDiscoveryDialog::on_ok(wxCommandEvent&)
{
    const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_results.size()) return;
    m_selected_ip       = m_results[sel].ip;
    m_selected_model    = m_results[sel].model;
    m_selected_birdwing = m_results[sel].is_birdwing;
    stop_discovery();
    EndModal(wxID_OK);
}

void MakerbotDiscoveryDialog::on_rescan(wxCommandEvent&)
{
    stop_discovery();
    start_discovery();
}

void MakerbotDiscoveryDialog::on_timer(wxTimerEvent&)
{
    // Heartbeat - currently unused, reserved for progress animation
}

} // namespace GUI
} // namespace Slic3r
