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
    // What the plan/minutes logic works from (no tokens in here)
    auto plan = me.contains("plan") ? me["plan"] : nlohmann::json();
    // Every number/bool at the top level (names and ids aside): where the
    // free minutes live differs between accounts
    std::string fields;
    for (auto it = me.begin(); it != me.end(); ++it)
      if (it->is_number() || it->is_boolean() || it->is_null())
        fields += " " + it.key() + "=" + it->dump();
    brls::Logger::info("AccountInfo fields:{}", fields);
    brls::Logger::info(
      "AccountInfo: plan={} plan_type={} price={} minutes_balance={}",
      plan.is_object() ? plan.value("name", std::string("?")) : std::string("none"),
      plan.is_object() && plan.contains("plan_type") ? plan["plan_type"].dump() : "-",
      plan.is_object() && plan.contains("price") ? plan["price"].dump() : "-",
      me.contains("minutes_balance") ? me["minutes_balance"].dump() : "-"
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
  if (inFlight.exchange(true))
    return; // a refresh is already running; its result is as fresh as ours would be

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

std::optional<int64_t> AccountInfo::secondsLeft(bool &unlimited) const {
  unlimited = false;
  if (!cached)
    return std::nullopt;
  const auto &me = *cached;
  auto plan = me.contains("plan") ? me["plan"] : nlohmann::json();
  // An unlimited plan is a paid one (plan_type 1). The free minutes live in
  // minutes_balance whatever the plan says.
  const bool freePlan = plan.is_object() && plan.contains("price") && plan["price"].is_string() &&
                        std::atof(plan["price"].get<std::string>().c_str()) <= 0.0;
  if (plan.is_object() && plan.value("plan_type", -1) == 1 && !freePlan) {
    unlimited = true;
    return std::nullopt;
  }
  if (!me.contains("minutes_balance") || !me["minutes_balance"].is_number())
    return std::nullopt;
  int64_t seconds = me["minutes_balance"].get<int64_t>() * 60;
  seconds -= std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - fetchedAt
  ).count();
  return std::max<int64_t>(seconds, 0);
}
