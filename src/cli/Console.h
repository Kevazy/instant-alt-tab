#pragma once

// Console output that keeps non-ASCII text intact: UTF-16 to a console window,
// UTF-8 when redirected to a file or pipe.

void Print(const wchar_t* format, ...);
// Like Print, without ending the line: for a question answered on the same line.
void Ask(const wchar_t* format, ...);
