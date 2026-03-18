#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <endstone/plugin/plugin.h>

namespace endstone {
class PlayerChatEvent;
}

class TgChatBridge : public endstone::Plugin {
public:
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

    void onPlayerChat(endstone::PlayerChatEvent &event);

private:
    struct Config {
        std::string bot_token;
        std::string chat_id;
        std::string game_to_tg_format;
        std::string tg_to_game_format;
        int max_game_to_tg = 256;
        int max_tg_to_game = 256;
        bool truncate_over_limit = true;
        int poll_timeout_sec = 25;
        int poll_interval_ms = 1000;
        int request_timeout_ms = 15000;
        int incoming_flush_ticks = 20;
        bool log_telegram_errors = true;
        bool ignore_commands = true;
    };

    struct TgIncomingMessage {
        std::string username;
        std::string text;
        std::string chat_id;
        std::string chat_username;
    };

    void loadOrCreateConfig();
    void startWorker();
    void stopWorker();
    void workerLoop();
    void senderLoop();
    void flushIncoming();

    std::string applyTemplate(const std::string &templ, const std::string &player, const std::string &username,
                              const std::string &message) const;
    std::string enforceLimit(const std::string &text, int max_len, bool &dropped) const;

    Config config_{};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::thread sender_;

    std::mutex outgoing_mutex_;
    std::deque<std::string> outgoing_queue_;
    std::condition_variable outgoing_cv_;

    std::mutex incoming_mutex_;
    std::deque<TgIncomingMessage> incoming_queue_;

    bool telegram_enabled_ = false;
    bool skip_existing_updates_ = true;
};
