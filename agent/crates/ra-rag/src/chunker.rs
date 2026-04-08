// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/chunker.rs
// MISSION: Language-aware function-boundary chunker for codebase ingest.
//          Splits source files at semantic boundaries (function definitions,
//          section headers) and prefixes each chunk with a file+symbol header
//          so the model can answer "where does X live?"
//
// Supported languages:
//   .hs              — Haskell: top-level definitions (column-0 identifiers)
//   .c / .cpp / .cu  — C/C++/CUDA: function bodies (brace-depth tracker)
//   .lc              — LambdaC: top-level let bindings
//   .md              — Markdown: section headers (## / #)
//   everything else  — Fallback: fixed-size paragraph chunks

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

/// Chunk a source file into semantically bounded pieces for embedding.
///
/// Each returned `Chunk` has:
///  - `symbol`  — the function/section name (or "" for prose chunks)
///  - `content` — the text to embed, prefixed with a location header
///
/// The caller supplies `rel_path` (repo-relative path as a display string).
pub fn chunk_file(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    let ext = rel_path.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    match ext.as_str() {
        "hs"           => chunk_haskell(source, rel_path, max_chars),
        "c" | "cpp" | "cu" | "h" => chunk_c(source, rel_path, max_chars),
        "lc"           => chunk_lambdac(source, rel_path, max_chars),
        "md"           => chunk_markdown(source, rel_path, max_chars),
        _              => chunk_fallback(source, rel_path, max_chars),
    }
}

/// A single chunk ready for embedding and storage.
#[derive(Debug, Clone)]
pub struct Chunk {
    /// Short symbol or section name. Empty for fallback chunks.
    pub symbol:  String,
    /// Full text to embed: includes a `[FILE: path | symbol]` header line.
    pub content: String,
}

impl Chunk {
    fn new(rel_path: &str, symbol: &str, body: &str) -> Self {
        let header = if symbol.is_empty() {
            format!("[FILE: {}]\n", rel_path)
        } else {
            format!("[FILE: {} | {}]\n", rel_path, symbol)
        };
        Self {
            symbol:  symbol.to_string(),
            content: format!("{}{}", header, body),
        }
    }
}

// ---------------------------------------------------------------------------
// Haskell chunker
// ---------------------------------------------------------------------------
// Strategy: every line at column 0 that begins with an alphabetic character
// (or `'`) AND is NOT a blank line or a comment starts a new top-level
// definition.  We group the type-signature line with the following definition
// body (which is indented or continues at col-0 with the same name).
//
// The symbol name is the first token on the starting line.

fn chunk_haskell(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    let lines: Vec<&str> = source.lines().collect();
    let mut chunks = Vec::new();
    let mut start = 0usize;
    let mut current_symbol = String::new();

    let is_toplevel_start = |line: &str| -> bool {
        if line.is_empty() { return false; }
        let first = line.chars().next().unwrap_or(' ');
        if !first.is_alphabetic() && first != '\'' { return false; }
        // Exclude module/import/where lines at top of file — they belong
        // together with the next definition or in their own small chunk.
        true
    };

    let extract_symbol = |line: &str| -> String {
        line.split_whitespace()
            .next()
            .unwrap_or("")
            .trim_end_matches(':')
            .to_string()
    };

    for i in 0..lines.len() {
        let line = lines[i];
        if i == 0 {
            current_symbol = extract_symbol(line);
            start = 0;
            continue;
        }
        if is_toplevel_start(line) {
            // Flush the chunk that just ended at i-1
            let body = lines[start..i].join("\n");
            if !body.trim().is_empty() {
                let chunks_from_body = split_if_large(&body, &current_symbol, rel_path, max_chars);
                chunks.extend(chunks_from_body);
            }
            start = i;
            current_symbol = extract_symbol(line);
        }
    }

    // Flush final chunk
    if start < lines.len() {
        let body = lines[start..].join("\n");
        if !body.trim().is_empty() {
            chunks.extend(split_if_large(&body, &current_symbol, rel_path, max_chars));
        }
    }

    chunks
}

// ---------------------------------------------------------------------------
// C / C++ / CUDA chunker
// ---------------------------------------------------------------------------
// Strategy: track brace depth.  When depth transitions from 0 → >0 we record
// the start of a function body.  When depth returns to 0 we end the chunk.
// The "signature" is the last non-empty column-0 line seen before the `{`.
//
// This handles most C/C++ patterns without a full parser.

fn chunk_c(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    let lines: Vec<&str> = source.lines().collect();
    let mut chunks  = Vec::new();
    let mut depth   = 0i32;
    let mut fn_start: Option<usize> = None;
    let mut fn_symbol = String::new();
    let mut last_col0_line = String::new();

    for (i, &line) in lines.iter().enumerate() {
        let trimmed = line.trim();

        // Track last column-0 non-blank line as candidate function signature.
        if !line.starts_with(' ') && !line.starts_with('\t') && !trimmed.is_empty() {
            last_col0_line = trimmed.to_string();
        }

        let opens:  i32 = line.chars().filter(|&c| c == '{').count() as i32;
        let closes: i32 = line.chars().filter(|&c| c == '}').count() as i32;
        let old_depth = depth;
        depth += opens - closes;

        if old_depth == 0 && opens > 0 {
            // Entering a new top-level block — this is a function start.
            fn_start  = Some(i.saturating_sub(4));   // include signature lines
            fn_symbol = extract_c_symbol(&last_col0_line);
        }

        if depth <= 0 && old_depth > 0 {
            depth = 0;
            if let Some(start) = fn_start.take() {
                let body = lines[start..=i].join("\n");
                if !body.trim().is_empty() {
                    chunks.extend(split_if_large(&body, &fn_symbol, rel_path, max_chars));
                }
            }
        }
    }

    // Any unclosed block (e.g., header-only) — flush as one chunk.
    if let Some(start) = fn_start {
        let body = lines[start..].join("\n");
        if !body.trim().is_empty() {
            chunks.extend(split_if_large(&body, &fn_symbol, rel_path, max_chars));
        }
    }

    // If nothing was emitted (all declarations, no function bodies), fall back.
    if chunks.is_empty() {
        return chunk_fallback(source, rel_path, max_chars);
    }

    chunks
}

fn extract_c_symbol(signature_line: &str) -> String {
    // Heuristic: find the last identifier before `(`
    // e.g. "static void handle_chat(uWS::WebSocket<false,true>* ws, ..."
    // → "handle_chat"
    if let Some(paren) = signature_line.find('(') {
        let before = &signature_line[..paren];
        before.split_whitespace()
              .last()
              .unwrap_or("")
              .trim_start_matches('*')
              .to_string()
    } else {
        signature_line.split_whitespace()
                      .last()
                      .unwrap_or("")
                      .to_string()
    }
}

// ---------------------------------------------------------------------------
// LambdaC chunker
// ---------------------------------------------------------------------------
// Strategy: same as Haskell — column-0 identifiers start new definitions.
// LambdaC uses `let name = ...` at top level or bare `name = expr`.

fn chunk_lambdac(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    // Reuse Haskell chunker — identical column-0 rule applies.
    chunk_haskell(source, rel_path, max_chars)
}

// ---------------------------------------------------------------------------
// Markdown chunker
// ---------------------------------------------------------------------------
// Strategy: split at `#` or `##` headers.  Each section becomes one chunk.
// The symbol is the header text (stripped of `#` and whitespace).

fn chunk_markdown(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    let lines: Vec<&str> = source.lines().collect();
    let mut chunks = Vec::new();
    let mut start = 0usize;
    let mut current_symbol = String::new();

    let is_header = |line: &str| line.starts_with('#');
    let header_symbol = |line: &str| -> String {
        line.trim_start_matches('#').trim().to_string()
    };

    for i in 0..lines.len() {
        if is_header(lines[i]) && i > 0 {
            let body = lines[start..i].join("\n");
            if !body.trim().is_empty() {
                chunks.extend(split_if_large(&body, &current_symbol, rel_path, max_chars));
            }
            start = i;
            current_symbol = header_symbol(lines[i]);
        } else if i == 0 && is_header(lines[i]) {
            current_symbol = header_symbol(lines[i]);
        }
    }

    if start < lines.len() {
        let body = lines[start..].join("\n");
        if !body.trim().is_empty() {
            chunks.extend(split_if_large(&body, &current_symbol, rel_path, max_chars));
        }
    }

    if chunks.is_empty() {
        return chunk_fallback(source, rel_path, max_chars);
    }

    chunks
}

// ---------------------------------------------------------------------------
// Fallback chunker — double-newline paragraph splitting
// ---------------------------------------------------------------------------

fn chunk_fallback(source: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    let mut chunks = Vec::new();
    let mut current = String::new();

    for para in source.split("\n\n") {
        let para = para.trim();
        if para.is_empty() { continue; }
        if current.is_empty() {
            current.push_str(para);
        } else if current.len() + para.len() + 2 <= max_chars {
            current.push_str("\n\n");
            current.push_str(para);
        } else {
            chunks.push(Chunk::new(rel_path, "", &current));
            current = para.to_string();
        }
    }
    if !current.is_empty() {
        chunks.push(Chunk::new(rel_path, "", &current));
    }
    chunks
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// If `body` exceeds `max_chars`, split it into sub-chunks preserving header.
/// Sub-chunks are tagged as `symbol[N]`.
fn split_if_large(body: &str, symbol: &str, rel_path: &str, max_chars: usize) -> Vec<Chunk> {
    if body.len() <= max_chars {
        return vec![Chunk::new(rel_path, symbol, body)];
    }

    // Split by lines into groups ≤ max_chars.
    let mut chunks = Vec::new();
    let mut current = String::new();
    let mut part = 0usize;

    for line in body.lines() {
        if current.len() + line.len() + 1 > max_chars && !current.is_empty() {
            let sub_sym = if symbol.is_empty() {
                format!("part{}", part)
            } else {
                format!("{}[{}]", symbol, part)
            };
            chunks.push(Chunk::new(rel_path, &sub_sym, &current));
            current.clear();
            part += 1;
        }
        if !current.is_empty() { current.push('\n'); }
        current.push_str(line);
    }

    if !current.is_empty() {
        let sub_sym = if part == 0 {
            symbol.to_string()
        } else {
            format!("{}[{}]", symbol, part)
        };
        chunks.push(Chunk::new(rel_path, &sub_sym, &current));
    }

    chunks
}
