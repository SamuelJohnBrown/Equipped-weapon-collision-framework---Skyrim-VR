#pragma once

// Skyrim VR and SKSE use Microsoft's C++ ABI.  The supported native build also
// targets that ABI, so this helper is normally a no-op.  It additionally keeps
// the source safe for an alternate Itanium-ABI build, whose virtual-destructor
// layout contains one extra entry before ReceiveEvent.  Engine-owned
// EventDispatcher instances then need the sink vptr advanced by one slot:
//
//   Itanium: [complete dtor] [deleting dtor] [ReceiveEvent]
//   MSVC:    [deleting dtor] [ReceiveEvent]
//
// The engine does not own or delete event sinks, so pointing at the deleting
// destructor entry gives Skyrim exactly the two slots it expects.  Plugin-owned
// dispatchers must keep the native Itanium vptr and do not use this helper.
template <class SinkT>
SinkT* MakeSkyrimEventSinkCompatible(SinkT* sink)
{
#if defined(__GXX_ABI_VERSION) && !defined(_MSC_VER)
    static bool adjusted = false;
    if (!adjusted && sink)
    {
        void*** objectVptr = reinterpret_cast<void***>(sink);
        *objectVptr = *objectVptr + 1;
        adjusted = true;
    }
#endif
    return sink;
}
