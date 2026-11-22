#include "core/account_info.hpp"
#include "core/vkpcapi.hpp"

AccountInfo &AccountInfo::instance() {
  static AccountInfo s;
  return s;
}

const std::optional<nlohmann::json> &AccountInfo::get() const { return cached; }

void AccountInfo::fetch() {
  try {
    auto me = VKPCAPI::usersMe();
    // performGet() yields null on HTTP/network errors, and the views read
    // name/session from this unchecked: don't cache or announce a non-account
    if (!me.is_object() || !me.contains("name") || !me.contains("session")) {
      brls::Logger::warning("AccountInfo: /users/me returned no usable account, ignoring");
      return;
    }
    this->cached = me;
    this->onUpdated.fire(me);
  } catch (const VKPCAPI::NotLoggedInException) {
    // noop
  }
}

void AccountInfo::clear() { this->cached = std::nullopt; }