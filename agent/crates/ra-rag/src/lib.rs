// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/lib.rs
// MISSION: ra-rag crate root — RAG pipeline public API.

pub mod types;       // SSoA LEVEL 0 — sovereign type anchor (authorize before modifying)
pub mod embedder;    // ONNX Runtime + gte-large-en-v1.5 + CUDA EP
pub mod retriever;   // usearch HNSW vector store
pub mod generator;   // OpenAI-compatible API (llama.cpp / Ollama / any endpoint)
pub mod pipeline;    // orchestrates embed → retrieve → generate
pub mod chunker;     // language-aware function-boundary chunker for codebase ingest
