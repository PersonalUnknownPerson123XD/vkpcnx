#pragma once
#include <atomic>
#include <chrono>
#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <optional>

class AccountInfo {
public:
  static AccountInfo &instance();
  const std::optional<nlohmann::json> &get() const;
  // Blocking /users/me request; UI thread only. Prefer fetchAsync().
  void fetch();
  // Refreshes /users/me off the UI thread and fires updated() on it when the
  // reply is a usable account. Does nothing while a refresh is already running.
  void fetchAsync();
  void clear();

  // Play time left according to /users/me as of now: minutes_balance minus the
  // time since it was fetched. nullopt: no account yet, or an unlimited paid plan
  // (the caller then shows "unlimited"); `unlimited` tells the two apart.
  std::optional<int64_t> secondsLeft(bool &unlimited) const;

  // Snapshot of the profile balance, frozen at the moment it is taken. The
  // stream counts down from this (profile_at_start - elapsed) and ignores the
  // 10-year placeholders the game server sends for free-minutes accounts.
  struct BalanceSnapshot {
    bool ready = false;
    bool unlimited = false;
    int64_t secondsAtStart = 0; // unused when unlimited
    std::string source;         // which field, for the log
  };
  BalanceSnapshot snapshotBalance() const;

  brls::Event<const nlohmann::json &> &updated() { return onUpdated; }

private:
  // Caches `me` and fires updated() if it looks like an account; UI thread only
  bool apply(const nlohmann::json &me);

  std::optional<nlohmann::json> cached;
  std::chrono::steady_clock::time_point fetchedAt{};
  std::atomic<bool> inFlight{false};
  brls::Event<const nlohmann::json &> onUpdated;
};
