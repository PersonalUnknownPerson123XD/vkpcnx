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

  brls::Event<const nlohmann::json &> &updated() { return onUpdated; }

private:
  // Caches `me` and fires updated() if it looks like an account; UI thread only
  bool apply(const nlohmann::json &me);

  std::optional<nlohmann::json> cached;
  std::chrono::steady_clock::time_point fetchedAt{};
  std::atomic<bool> inFlight{false};
  brls::Event<const nlohmann::json &> onUpdated;
};
