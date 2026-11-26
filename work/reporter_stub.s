// Stub replacement for libblackhub_event_reporter.so
// Exports all symbols the client + Java need, but does nothing (no Sentry, no telemetry).
// Three semantics: ret (no-op / return this), ret0 (return 0/false), retobj (return fake static object).

    .text

    .macro NOOP name
    .global \name
    .type \name, @function
\name:
    ret
    .endm

    .macro RET0 name
    .global \name
    .type \name, @function
\name:
    mov w0, #0
    ret
    .endm

    .macro RETOBJ name
    .global \name
    .type \name, @function
\name:
    adrp x0, stub_obj
    add  x0, x0, :lo12:stub_obj
    ret
    .endm

// ---- C++ symbols needed by libblackrussia-client.so (41) ----

// EventFilter
    RET0   _ZN8blackhub14event_reporter11EventFilter5emptyEv
    RETOBJ _ZN8blackhub14event_reporter11EventFilter8defaultsEv
    NOOP   _ZN8blackhub14event_reporter11EventFilterC1EOS1_
    NOOP   _ZN8blackhub14event_reporter11EventFilterD1Ev
    NOOP   _ZN8blackhub14event_reporter11EventFilteraSEOS1_
    NOOP   _ZN8blackhub14event_reporter11EventFilter7BuilderC1Ev
    NOOP   _ZN8blackhub14event_reporter11EventFilter7BuilderD1Ev
    NOOP   _ZN8blackhub14event_reporter11EventFilter7Builder4rateENSt6__ndk117basic_string_viewIcNS3_11char_traitsIcEEEENS0_6ELevelEd
    NOOP   _ZN8blackhub14event_reporter11EventFilter7Builder5levelENSt6__ndk117basic_string_viewIcNS3_11char_traitsIcEEEENS0_6ELevelEb
    NOOP   _ZNK8blackhub14event_reporter11EventFilter7Builder5buildEv

// RuntimeConfig
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfigC1Ev
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfigD1Ev
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig6setDsnENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig7setDistENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig10setReleaseENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig14setEnvironmentENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig10setCaCertsERKNSt6__ndk14__fs10filesystem4pathE
    NOOP   _ZN8blackhub14event_reporter13RuntimeConfig15setDatabasePathERKNSt6__ndk14__fs10filesystem4pathE

// Event / Value (fluent API: tag/extra/context return Event& -> x0=this)
    NOOP   _ZN8blackhub14event_reporter5EventD1Ev
    NOOP   _ZNR8blackhub14event_reporter5Event3tagENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEES6_
    NOOP   _ZNR8blackhub14event_reporter5Event5extraENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEERKNS0_5ValueE
    NOOP   _ZNR8blackhub14event_reporter5Event7contextENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEERKNS0_5ValueE
    NOOP   _ZN8blackhub14event_reporter5ValueC1Ev
    NOOP   _ZN8blackhub14event_reporter5ValueC1Eb
    NOOP   _ZN8blackhub14event_reporter5ValueC1ENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter5ValueC1ERKNSt6__ndk14spanIKNS2_4pairINS2_17basic_string_viewIcNS2_11char_traitsIcEEEES1_EELm18446744073709551615EEE
    NOOP   _ZN8blackhub14event_reporter5ValueC2ENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE
    NOOP   _ZN8blackhub14event_reporter5ValueC2El
    NOOP   _ZN8blackhub14event_reporter5ValueC2Em

// Logger (enabled->false gates everything; message/breadcrumb no-op)
    NOOP   _ZN8blackhub14event_reporter6LoggerD1Ev
    RET0   _ZNK8blackhub14event_reporter6Logger7enabledENS0_6ELevelE
    NOOP   _ZNK8blackhub14event_reporter6Logger7messageENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEENS0_6ELevelE
    NOOP   _ZN8blackhub14event_reporter6Logger10breadcrumbENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEENS0_6ELevelERKNS0_5ValueE

// Runtime / Registry (return fake objects so callers can chain no-op methods)
    NOOP   _ZN8blackhub14event_reporter7Runtime10initializeERKNS0_13RuntimeConfigE
    RET0   _ZNK8blackhub14event_reporter7Runtime11initializedEv
    NOOP   _ZN8blackhub14event_reporter7Runtime14setLogCallbackERKNSt6__ndk18functionIFvNS2_17basic_string_viewIcNS2_11char_traitsIcEEEES7_EEE
    NOOP   _ZN8blackhub14event_reporter7Runtime7setUserERKNSt6__ndk18optionalINS0_4UserEEE
    RETOBJ _ZN8blackhub14event_reporter7Runtime8instanceEv
    RETOBJ _ZN8blackhub14event_reporter7Runtime8registryEv
    NOOP   _ZN8blackhub14event_reporter8Registry5applyERKNS0_11EventFilterE
    RETOBJ _ZN8blackhub14event_reporter8Registry6ensureENSt6__ndk117basic_string_viewIcNS2_11char_traitsIcEEEE

    .macro RET1 name
    .global \name
    .type \name, @function
\name:
    mov w0, #1
    ret
    .endm

// ---- JNI symbols for com.blackhub.EventReporter.Native ----
    NOOP   Java_com_blackhub_EventReporter_Native_init
    RET1   Java_com_blackhub_EventReporter_Native_create
    NOOP   Java_com_blackhub_EventReporter_Native_destroy
    RET1   Java_com_blackhub_EventReporter_Native_alive
    RET0   Java_com_blackhub_EventReporter_Native_enabled
    NOOP   Java_com_blackhub_EventReporter_Native_breadcrumb
    NOOP   Java_com_blackhub_EventReporter_Native_tag
    NOOP   Java_com_blackhub_EventReporter_Native_context
    NOOP   Java_com_blackhub_EventReporter_Native_message
    NOOP   Java_com_blackhub_EventReporter_Native_event

    .data
    .align 4
stub_obj:
    .space 256
