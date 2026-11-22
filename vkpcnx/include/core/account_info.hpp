#pragma once
#include <atomic>
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

  brls::Event<const nlohmann::json &> &updated() { return onUpdated; }

private:
  // Caches `me` and fires updated() if it looks like an account; UI thread only
  bool apply(const nlohmann::json &me);

  std::optional<nlohmann::json> cached;
  std::atomic<bool> inFlight{false};
  brls::Event<const nlohmann::json &> onUpdated;
};
