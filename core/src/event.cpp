#include "evk/event.h"

namespace {

evk::EventFunc g_eventFunc = nullptr;

} // namespace

namespace evk {

void setEventFunc(EventFunc func) {
    g_eventFunc = func;
}

void dispatchEvent(EventId id, const void* data) {
    if (g_eventFunc) {
        g_eventFunc(id, data);
    }
}

} // namespace evk
