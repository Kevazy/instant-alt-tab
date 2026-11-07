#pragma once

// Console output that keeps non-ASCII text intact: UTF-16 to a console window,
// UTF-8 when redirected to a file or pipe.
//
// Formats may mark text with styles: <b>bold</>, <good>aqua</>, <warn>yellow</>,
// <bad>red</> and <dim>grey</> (</> ends any of them). They are shown where the
// console understands colour sequences (any Windows 11 console, in PowerShell 5
// and 7 alike, but not the legacy console) and NO_COLOR isn't set; elsewhere,
// and when redirected, the marks are left out. Only formats are styled, never
// the arguments.

void Print(const wchar_t* format, ...);
// Like Print, without ending the line: for a question answered on the same line.
void Ask(const wchar_t* format, ...);
// Clears the console window and its scrollback; does nothing when output is redirected.
void ClearScreen();
// Clears the line the cursor is on and moves to its start, so it can be written
// again; returns false (and does nothing) where the console can't.
bool ClearLine();
// The console window's width in columns, or 0 when output is redirected.
int ConsoleColumns();
