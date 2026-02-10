#include "telnet_sml_app.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#include "client.hpp"
#include "ser_database.hpp"
#include "telnet_fsm.hpp"
#include "thread_manager.hpp"
#include "ws_server.hpp"

using namespace sml;

namespace
{
class FsmWorker
{
    std::atomic<bool> stop_flag_{false};
    std::thread worker_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<bool> run_queue_;

    sml::sm<TelnetFSM> fsm_;
    RetryState& retry_;
    std::atomic<bool>& app_running_;

    void runCycle(bool reset_retry)
    {
        if (!app_running_.load())
            return;

        if (reset_retry)
            retry_.reset();

        fsm_.process_event(start_event{});

        for (int i = 0; i < 10 && app_running_.load(); ++i)
        {
            bool handled = fsm_.process_event(step_event{});
            if (!handled)
                fsm_.process_event(unhandled_event{});

            if (fsm_.is("Error"_s))
            {
                std::cout << "[ERROR] FSM entered Error state!\n";
                break;
            }

            if (fsm_.is("Done"_s))
            {
                std::cout << "[INFO] SER data retrieved successfully.\n";
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void runLoop()
    {
        while (!stop_flag_.load())
        {
            bool reset_retry = false;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [&]() {
                    return stop_flag_.load() || !run_queue_.empty();
                });

                if (stop_flag_.load())
                    break;

                reset_retry = run_queue_.front();
                run_queue_.pop_front();
            }

            runCycle(reset_retry);
        }
    }

public:
    FsmWorker(TelnetClient& client,
              ConnectionConfig& conn,
              LoginConfig& creds,
              RetryState& retry,
              SERDatabase& db,
              std::atomic<bool>& app_running)
        : fsm_{ client, conn, creds, retry, db }
        , retry_(retry)
        , app_running_(app_running)
    {
    }

    void start()
    {
        stop_flag_ = false;
        worker_thread_ = std::thread([this]() { runLoop(); });
    }

    void stop()
    {
        stop_flag_ = true;
        queue_cv_.notify_all();
        if (worker_thread_.joinable())
            worker_thread_.join();
    }

    void requestRun(bool reset_retry)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            run_queue_.push_back(reset_retry);
        }
        queue_cv_.notify_one();
    }
};
}  // namespace

class TelnetSmlApp::Impl
{
public:
    std::atomic<bool> app_running{true};
    TelnetClient client;

    ConnectionConfig conn{
        "192.168.0.2",
        23,
        std::chrono::milliseconds(2000)
    };

    LoginConfig creds{
        "acc",
        "OTTER"
    };

    RetryState retry{3, 0, std::chrono::seconds(30)};
    SERDatabase serDb{"ser_records.db"};
    SERWebSocketServer wsServer{serDb, 8765};
    ThreadManager threadMgr{serDb, std::chrono::seconds(120)};
    std::unique_ptr<FsmWorker> fsmWorker;
    bool running = false;

    bool start()
    {
        if (running)
            return true;

        app_running = true;

        std::cout << "========================================\n";
        std::cout << "  Telnet-SML Multi-threaded Application\n";
        std::cout << "========================================\n\n";

        if (!serDb.open())
        {
            std::cerr << "Failed to open database: " << serDb.getLastError() << "\n";
            return false;
        }
        std::cout << "[DB] Database opened. Existing records: " << serDb.getRecordCount() << "\n";

        if (!wsServer.start())
        {
            std::cerr << "Failed to start WebSocket server\n";
            serDb.close();
            return false;
        }

        fsmWorker = std::make_unique<FsmWorker>(client, conn, creds, retry, serDb, app_running);
        fsmWorker->start();

        threadMgr.setPollingCallback([this]() {
            if (!app_running.load())
                return;

            std::cout << "[Poller] Re-running FSM for SER update...\n";
            fsmWorker->requestRun(true);
            std::cout << "[Poller] Poll cycle complete\n";
        });

        threadMgr.startAll();

        std::cout << "\n[Main] Starting initial SER retrieval...\n";
        fsmWorker->requestRun(false);

        std::cout << "\n========================================\n";
        std::cout << "  Active Threads:\n";
        std::cout << "  - Main Thread: User input handler\n";
        std::cout << "  - Thread 1: WebSocket Server (port 8765)\n";
        std::cout << "  - Thread 2: FSM Worker (SER retrieval)\n";
        std::cout << "  - Thread 3: SER Poller (2 min interval)\n";
        std::cout << "  - Thread 4: Database Writer (async)\n";
        std::cout << "========================================\n";

        running = true;
        return true;
    }

    void waitForExit()
    {
        std::cout << "\n[INFO] Press Enter to exit...\n";
        std::cin.get();
    }

    void stop()
    {
        if (!running)
            return;

        std::cout << "\n[Main] Shutting down...\n";
        app_running = false;

        threadMgr.stopAll();
        if (fsmWorker)
            fsmWorker->stop();
        wsServer.stop();
        serDb.close();

        running = false;
        std::cout << "[Main] Application terminated.\n";
    }
};

TelnetSmlApp::TelnetSmlApp() : impl_(std::make_unique<Impl>())
{
}

TelnetSmlApp::~TelnetSmlApp()
{
    if (impl_)
        impl_->stop();
}

bool TelnetSmlApp::start()
{
    return impl_->start();
}

void TelnetSmlApp::waitForExit()
{
    impl_->waitForExit();
}

void TelnetSmlApp::stop()
{
    impl_->stop();
}

bool TelnetSmlApp::isRunning() const
{
    return impl_ && impl_->running;
}
