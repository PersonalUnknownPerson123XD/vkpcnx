#include "core/account_info.hpp"
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
