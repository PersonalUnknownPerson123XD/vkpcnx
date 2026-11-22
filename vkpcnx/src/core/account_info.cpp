#include "core/account_info.hpp"
#include <cstdlib>
#include "core/utils/thread.hpp"
#include "core/vkpcapi.hpp"

AccountInfo &AccountInfo::instance() {
  static AccountInfo s;
  return s;
}

const std::optional<nlohmann::json> &AccountInfo::get() const { return cached; }

bool AccountInfo::apply(const nlohmann::json &me) {
  // performGet() yields null on HTTP/network errors, and the views read
  // name/session from this unchecked: don't cache or announce a non-account
  if (!me.is_object() || !me.contains("name") || !me.contains("session")) {
    brls::Logger::warning("AccountInfo: /users/me returned no usable account, ignoring");
    return false;
  }
  this->cached = me;
  this->fetchedAt = std::chrono::steady_clock::now();
  {
    auto plan = me.contains("plan") ? me["plan"] : nlohmann::json();
    // Every number/bool at the top level and one object level down (plan,
    // subscription, balances…). Skip strings so tokens never land in the log.
    std::string fields;
    auto appendNums = [&](const nlohmann::json &obj, const std::string &prefix) {
      if (!obj.is_object())
        return;
      for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it->is_number() || it->is_boolean() || it->is_null())
          fields += " " + prefix + it.key() + "=" + it->dump();
      }
    };
    appendNums(me, "");
    for (auto it = me.begin(); it != me.end(); ++it)
      if (it->is_object())
        appendNums(*it, it.key() + ".");
    brls::Logger::info("AccountInfo fields:{}", fields);
    brls::Logger::info(
      "AccountInfo: plan={} plan_type={} price={} minutes_balance={} overlimit_minutes_balance={}",
      plan.is_object() ? plan.value("name", std::string("?")) : std::string("none"),
      plan.is_object() && plan.contains("plan_type") ? plan["plan_type"].dump() : "-",
      plan.is_object() && plan.contains("price") ? plan["price"].dump() : "-",
      me.contains("minutes_balance") ? me["minutes_balance"].dump() : "-",
      me.contains("overlimit_minutes_balance") ? me["overlimit_minutes_balance"].dump() : "-"
    );
  }
  this->onUpdated.fire(me);
  return true;
}

void AccountInfo::fetch() {
  try {
    apply(VKPCAPI::usersMe());
  } catch (const VKPCAPI::NotLoggedInException &) {
    // noop
  } catch (const std::exception &e) {
    brls::Logger::warning("AccountInfo: fetch failed: {}", e.what());
  }
}

void AccountInfo::fetchAsync() {
  if (inFlight.exchange(true)) {
    brls::Logger::info("AccountInfo: GET /users/me skipped, a refresh is already running");
    return;
  }
  brls::Logger::info("AccountInfo: GET /users/me (async)");

  vkpcnx::utils::runDetached([this] {
    nlohmann::json me;
    try {
      me = VKPCAPI::usersMe();
    } catch (const VKPCAPI::NotLoggedInException &) {
      // noop
    } catch (const std::exception &e) {
      brls::Logger::warning("AccountInfo: refresh failed: {}", e.what());
    }
    brls::sync([this, me = std::move(me)] {
      inFlight = false;
      if (!me.is_null())
        apply(me);
    });
  });
}

void AccountInfo::clear() { this->cached = std::nullopt; }

namespace {
bool jsonInt64(const nlohmann::json &v, int64_t &out) {
  if (v.is_number_integer()) {
    out = v.get<int64_t>();
    return true;
  }
  if (v.is_number_float()) {
    out = static_cast<int64_t>(v.get<double>());
    return true;
  }
  if (v.is_string()) {
    try {
      size_t n = 0;
      auto s = v.get<std::string>();
      long long x = std::stoll(s, &n);
      if (n == s.size()) {
        out = x;
        return true;
      }
    } catch (...) {
    }
  }
  return false;
}

// Official web client: minutes_balance + overlimit_minutes_balance (сверхлимит).
// Ignore 10-year / huge placeholders; «Неогранич.» only for a paid plan_type 1.
constexpr int64_t kHugeMinutes = 7LL * 24 * 60;
} // namespace

std::optional<int64_t> AccountInfo::secondsLeft(bool &unlimited) const {
  unlimited = false;
  if (!cached)
    return std::nullopt;
  const auto &me = *cached;
  auto plan = me.contains("plan") ? me["plan"] : nlohmann::json();
  const bool freePlan = plan.is_object() && plan.contains("price") &&
                        ((plan["price"].is_string() &&
                          std::atof(plan["price"].get<std::string>().c_str()) <= 0.0) ||
                         (plan["price"].is_number() && plan["price"].get<double>() <= 0.0));
  // Paid monthly unlimited only. Comfort (plan_type 2) still has a day balance.
  if (plan.is_object() && plan.value("plan_type", -1) == 1 && !freePlan) {
    unlimited = true;
    return std::nullopt;
  }

  int64_t minutes = 0;
  bool have = false;
  auto addMinutes = [&](const nlohmann::json &obj, const char *key) {
    if (!obj.is_object() || !obj.contains(key))
      return;
    int64_t v = 0;
    if (!jsonInt64(obj[key], v))
      return;
    if (v >= kHugeMinutes)
      return;
    minutes += v;
    have = true;
  };
  addMinutes(me, "minutes_balance");
  addMinutes(me, "overlimit_minutes_balance");
  addMinutes(me, "remaining_minutes");
  addMinutes(me, "minutes_left");
  if (plan.is_object()) {
    addMinutes(plan, "minutes_balance");
    addMinutes(plan, "overlimit_minutes_balance");
  }

  int64_t seconds = 0;
  auto takeSeconds = [&](const nlohmann::json &obj, const char *key) {
    if (have || !obj.is_object() || !obj.contains(key))
      return;
    int64_t v = 0;
    if (!jsonInt64(obj[key], v))
      return;
    if (v >= kHugeMinutes * 60)
      return;
    seconds = v;
    have = true;
  };
  takeSeconds(me, "seconds_left");
  takeSeconds(me, "time_left_seconds");
  takeSeconds(me, "remaining_seconds");
  takeSeconds(me, "time_left"); // last: official UI uses minutes; this may be either unit

  if (!have)
    return std::nullopt;
  if (seconds == 0 && minutes == 0 &&
      !(me.contains("minutes_balance") || me.contains("overlimit_minutes_balance"))) {
    // took time_left-as-seconds path with value already in `seconds`, or zero minutes
  }
  if (seconds == 0)
    seconds = minutes * 60;
  seconds -= std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - fetchedAt
  )
               .count();
  return std::max<int64_t>(seconds, 0);
}

AccountInfo::BalanceSnapshot AccountInfo::snapshotBalance() const {
  BalanceSnapshot snap;
  bool unlimited = false;
  auto left = secondsLeft(unlimited);
  snap.unlimited = unlimited;
  if (unlimited) {
    snap.ready = true;
    snap.source = "unlimited plan_type 1";
    return snap;
  }
  if (!left)
    return snap;
  snap.ready = true;
  snap.secondsAtStart = *left;
  const auto &me = *cached;
  std::string src;
  auto mention = [&](const char *k) {
    if (me.contains(k)) {
      if (!src.empty())
        src += "+";
      src += k;
    }
  };
  mention("minutes_balance");
  mention("overlimit_minutes_balance");
  mention("seconds_left");
  mention("time_left_seconds");
  mention("remaining_seconds");
  mention("time_left");
  snap.source = src.empty() ? "profile" : src;
  return snap;
}
