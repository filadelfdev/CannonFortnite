#pragma once
//
// cf_api.h — cross-module linkage annotations.
//
// Each DLL is built with its own -DCF_BUILDING_<NAME>. When a header
// is pulled into the *producing* DLL, CF_API becomes dllexport; when
// pulled into any *consuming* DLL, it becomes dllimport. On non-MSVC
// toolchains (local linting, etc.) the macros expand to nothing.
//
// Keyed per-module to let a single header declare symbols from
// multiple DLLs (e.g. a tick that lives in features but reads a
// getter that lives in core).
//

#if defined(_MSC_VER)
  #define CF_EXPORT __declspec(dllexport)
  #define CF_IMPORT __declspec(dllimport)
#else
  #define CF_EXPORT
  #define CF_IMPORT
#endif

// --- cheat_core.dll ---------------------------------------------------
#if defined(CF_BUILDING_CORE)
  #define CF_CORE_API CF_EXPORT
#else
  #define CF_CORE_API CF_IMPORT
#endif

// --- cheat_antiban.dll ------------------------------------------------
#if defined(CF_BUILDING_ANTIBAN)
  #define CF_AB_API CF_EXPORT
#else
  #define CF_AB_API CF_IMPORT
#endif

// --- cheat_ui.dll -----------------------------------------------------
#if defined(CF_BUILDING_UI)
  #define CF_UI_API CF_EXPORT
#else
  #define CF_UI_API CF_IMPORT
#endif

// --- cheat_features.dll -----------------------------------------------
#if defined(CF_BUILDING_FEATURES)
  #define CF_FEAT_API CF_EXPORT
#else
  #define CF_FEAT_API CF_IMPORT
#endif

// Generic CF_API — expands to nothing. Use the per-module macro for
// anything that crosses a module boundary.
#define CF_API
