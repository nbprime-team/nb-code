#!/usr/bin/env python3
"""strip_prose.py -- deploy main.py without its prose.

WHY
    The calculator's Python environment compiles a module's whole source into its
    own memory, and generously: primeLua measured 156 KB of source costing about
    6 MB of heap on this machine, 40% of it comments.  nbCode's launcher is 36 KB
    of heavily commented Python, and the comments are the reason the file is
    readable -- so they stay in the repository and are dropped from the copy that
    ships.

WHAT IT DOES
    * every `#` comment goes;
    * every docstring goes, EXCEPT the module's first paragraph (that is what
      `help(main)` shows on the calculator);
    * a block whose only statement was a docstring gets `pass` instead, or it
      would not parse;
    * the result is COMPILED before it is written: a stripped launcher that does
      not parse would be a calculator that cannot start.

    Nothing is deleted from the repository: calc/main.py keeps the prose, and
    `make deploy` runs this on the way out.

USAGE
    python3 tools/strip_prose.py calc/main.py build/main.deploy.py
    python3 tools/strip_prose.py calc/main.py -            # to stdout
"""
import io
import os
import sys
import tokenize

KEEP_MODULE_DOC = 400          # characters of the module docstring to keep
DQ = '"' * 3
SQ = "'" * 3


def strip(source):
    """`source` with comments and docstrings removed.

    Line-based, COLUMN-ACCURATE editing of the token stream's spans: whatever is
    not a comment or a docstring is left byte for byte (indentation, blank lines,
    string literals that are real values), so nothing is reformatted by accident.
    """
    lines = source.splitlines(True)
    toks = list(tokenize.generate_tokens(io.StringIO(source).readline))
    edits = []                              # (start, end, replacement)
    for i, tok in enumerate(toks):
        kind, text, start, end, _line = tok
        if kind == tokenize.COMMENT:
            edits.append((start, end, ''))
            continue
        if kind != tokenize.STRING:
            continue
        # A DOCSTRING is a string that is a whole statement: the token before it
        # ends a line (or opens a block) and the token after it closes one.
        prev_kind = toks[i - 1][0] if i else tokenize.INDENT
        if prev_kind not in (tokenize.INDENT, tokenize.NEWLINE, tokenize.NL,
                             tokenize.DEDENT):
            continue
        # A DOCSTRING ENDS A STATEMENT, so the next token is a NEWLINE.  A string
        # that is only a FRAGMENT of an implicit concatenation is followed by NL
        # instead (the line break sits inside brackets), and treating those as
        # docstrings DELETED THEIR CONTINUATION LINES: the deployed launcher lost
        # the second half of
        #     log('[hook] ... 0x%08X mapped (hook calls in '
        #         'flight: %r, ...) until the next load' % (...))
        # and died at that very line with "not all arguments converted during
        # string formatting".  Only NEWLINE means "this string is a statement".
        next_kind = toks[i + 1][0] if i + 1 < len(toks) else tokenize.ENDMARKER
        if next_kind != tokenize.NEWLINE:
            continue
        if start[0] == 1:
            # THE MODULE DOCSTRING: its first paragraph is kept, because that is
            # what `help(main)` shows on the calculator.
            quote = DQ if text.startswith(DQ) else SQ
            inner = text[3:-3] if text.endswith(quote) else text[3:]
            cut = inner.find('\n\n')
            if cut < 0:
                cut = min(len(inner), KEEP_MODULE_DOC)
            edits.append((start, end, quote + inner[:cut].strip('\n') + quote))
            continue
        # Is the docstring the block's ONLY statement?  Removing it would leave
        # an empty suite, which does not parse: it becomes `pass`.
        j = i + 1
        while j < len(toks) and toks[j][0] in (tokenize.NEWLINE, tokenize.NL):
            j += 1
        alone = (j >= len(toks)
                 or toks[j][0] in (tokenize.DEDENT, tokenize.ENDMARKER))
        edits.append((start, end, 'pass' if alone else ''))

    # back to front, so every earlier offset is still valid
    for (start, end, repl) in sorted(edits, key=lambda e: e[0], reverse=True):
        (sr, sc), (er, ec) = start, end
        if sr == er:
            line = lines[sr - 1]
            lines[sr - 1] = line[:sc] + repl + line[ec:]
        else:
            first = lines[sr - 1][:sc] + repl
            last = lines[er - 1][ec:]
            lines[sr - 1:er] = [first + last]
    # The edits leave the spaces that used to sit before a comment behind.  They
    # are removed -- EXCEPT on lines that are part of a multi-line string, where
    # trailing whitespace is file CONTENT and not layout.
    inside = set()
    for tok in toks:
        if tok[0] == tokenize.STRING and tok[2][0] != tok[3][0]:
            for row in range(tok[2][0], tok[3][0] + 1):
                inside.add(row)
    for i, line in enumerate(lines):
        if (i + 1) not in inside:
            nl = '\n' if line.endswith('\n') else ''
            body = line[:-1] if nl else line
            lines[i] = body.rstrip(' \t\r') + nl
    out = ''.join(lines)
    while '\n\n\n\n' in out:                # blank runs cost bytes, say nothing
        out = out.replace('\n\n\n\n', '\n\n\n')
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip().split('USAGE')[1].strip())
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src) as f:
        source = f.read()
    stripped = strip(source)
    try:
        compile(stripped, dst if dst != '-' else '<stripped>', 'exec')
    except SyntaxError as e:
        print('strip_prose: REFUSING TO WRITE: the result does not parse: %s'
              % (e,))
        return 1
    if stripped.lstrip()[:3] not in (DQ, SQ):
        print('strip_prose: REFUSING TO WRITE: the module docstring is gone')
        return 1
    if dst == '-':
        sys.stdout.write(stripped)
        return 0
    with open(dst, 'w') as f:
        f.write(stripped)
    saved = len(source) - len(stripped)
    print('    main.py: %d B -> %d B (prose removed: %d B, %.0f%%)'
          % (len(source), len(stripped), saved,
             100.0 * saved / max(1, len(source))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
