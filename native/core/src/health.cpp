#include "bg3vr/health.hpp"

#include "bg3vr/log.hpp"

#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace bg3vr::health {

namespace {

std::mutex g_mutex;
std::map<std::string, std::pair<State, std::string>, std::less<>> g_features; // under g_mutex

const char* name(State state) {
    switch (state) {
    case State::Ready: return "ready";
    case State::Off: return "off";
    default: return "UNAVAILABLE";
    }
}

} // namespace

void set(std::string_view feature, State state, std::string_view detail) {
    std::scoped_lock lock(g_mutex);
    g_features[std::string(feature)] = {state, std::string(detail)};
}

void log_summary(std::string_view when) {
    std::scoped_lock lock(g_mutex);
    int ready = 0, off = 0, unavailable = 0;
    for (const auto& [feature, entry] : g_features) {
        (entry.first == State::Ready ? ready : entry.first == State::Off ? off : unavailable)++;
    }
    log::info("health ({}): {} ready, {} off, {} unavailable", when, ready, off, unavailable);
    for (const auto& [feature, entry] : g_features) {
        if (entry.first == State::Unavailable) {
            log::warn("health:   {:<22} {}{}{}", feature, name(entry.first), entry.second.empty() ? "" : ": ", entry.second);
        } else {
            log::info("health:   {:<22} {}{}{}", feature, name(entry.first), entry.second.empty() ? "" : ": ", entry.second);
        }
    }
}

} // namespace bg3vr::health
