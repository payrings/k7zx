// k7zx 5.0 - modern C++ port
//
// User-facing strings: technique names, per-method explanations and the
// three conversion-mode blurbs, transcribed from the original's
// textos.cpp (it is texts.cpp here).
#ifndef K7ZX_TEXTS_H
#define K7ZX_TEXTS_H

#include "defs.h"

namespace k7zx {
namespace texts {

/// Human readable name of a loading method, exactly as the original spelled
/// it in its `tecnica[]` table.  These strings are part of the interface and
/// are not to be reworded.
const char* methodName(Method m);

/// The main window's radio captions for the same techniques.  The original
/// spelled "Shavings Slow"/"Shavings Delta" here but "S. Slow"/"S. Delta" in
/// its options dialog, so both are reproduced rather than normalised.
const char* methodCaption(Method m);

/// Short tag used when embedding the method in an output file name.
const char* methodTag(Method m);

/// Short tag for the loading scheme.
const char* schemeTag(Scheme s);

/// Short tag for the samples-per-bit setting, e.g. "2.75".
const char* samplesTag(int samplesPerBit);

/// Long-form explanation of a loading method.
const char* explanation(Method m);

/// Blurb describing conversion mode `mode` (normal / hispeed).
const char* conversionMode(int mode);

}  // namespace texts
}  // namespace k7zx

#endif  // K7ZX_TEXTS_H
