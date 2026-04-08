// SSoA LEVEL 0: SOVEREIGN ANCHOR — IMMUTABLE LAW
// FILE: crates/ra-rag/src/types.rs
// MISSION: Core type definitions for the RusticAgentic RAG pipeline.
//          ALL other ra-rag modules (embedder, retriever, generator, pipeline)
//          derive from these types. Every struct, enum, and alias defined here
//          is load-bearing. DO NOT modify without Captain authorization and a
//          full downstream audit of embedder → retriever → generator → pipeline.

use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};
use std::fmt;
use uuid::Uuid;

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

/// Opaque identifier for a single encrypted vault shard.
/// Wraps a string key (GCS object path or local file path).
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct ShardId(pub String);

impl ShardId {
    pub fn new(id: impl Into<String>) -> Self {
        Self(id.into())
    }
}

impl fmt::Display for ShardId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}

// ---------------------------------------------------------------------------
// Document model
// ---------------------------------------------------------------------------

/// Provenance metadata attached to every document chunk stored in the vault.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DocumentMetadata {
    /// Original file path or GCS object key of the source document.
    pub source: String,
    /// Vault shard this chunk was ingested from.
    pub shard_id: ShardId,
    /// Zero-based index of this chunk within its source document.
    pub chunk_index: u32,
    /// Total number of chunks produced from the source document.
    pub total_chunks: u32,
}

/// A single text chunk stored in the vault and indexed for retrieval.
/// This is the atomic unit of the RusticAgentic knowledge base.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Document {
    /// Stable unique identifier assigned at ingest time.
    pub id: Uuid,
    /// Raw text content of this chunk.
    pub content: String,
    /// Provenance information.
    pub metadata: DocumentMetadata,
}

impl Document {
    pub fn new(content: impl Into<String>, metadata: DocumentMetadata) -> Self {
        Self {
            id: Uuid::new_v4(),
            content: content.into(),
            metadata,
        }
    }
}

// ---------------------------------------------------------------------------
// Embedding
// ---------------------------------------------------------------------------

/// A float32 embedding vector produced by the ONNX embedding engine.
/// Vectors are always L2-normalized before storage and search.
#[derive(Debug, Clone)]
pub struct Embedding {
    /// L2-normalized float32 vector. Length == `dimension`.
    pub vector: Vec<f32>,
    /// Dimensionality of the vector space.
    /// gte-large-en-v1.5 produces 1024-dimensional embeddings.
    pub dimension: usize,
    /// HuggingFace model ID that produced this embedding.
    pub model: String,
}

impl Embedding {
    pub fn new(vector: Vec<f32>, model: impl Into<String>) -> Self {
        let dimension = vector.len();
        Self {
            vector,
            dimension,
            model: model.into(),
        }
    }
}

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------

/// An inbound query from the user, submitted through the gateway.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Query {
    /// Stable identifier for this query — used in audit log entries.
    pub id: Uuid,
    /// The user's natural language question.
    pub text: String,
    /// Number of document chunks to retrieve from the vector store.
    pub top_k: usize,
    /// Wall-clock time when the query was received.
    pub created_at: DateTime<Utc>,
}

impl Query {
    pub fn new(text: impl Into<String>) -> Self {
        Self {
            id: Uuid::new_v4(),
            text: text.into(),
            top_k: 10,
            created_at: Utc::now(),
        }
    }

    pub fn with_top_k(mut self, k: usize) -> Self {
        self.top_k = k;
        self
    }
}

// ---------------------------------------------------------------------------
// Retrieval results
// ---------------------------------------------------------------------------

/// A document returned from the vector search, paired with its similarity score.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RetrievedDocument {
    /// The matched document chunk.
    pub document: Document,
    /// Cosine similarity score in [0.0, 1.0]. Higher = more relevant.
    pub score: f32,
    /// 1-based rank in the result set (rank 1 = most relevant).
    pub rank: u32,
}

// ---------------------------------------------------------------------------
// Pipeline output
// ---------------------------------------------------------------------------

/// The complete response from a single RAG pipeline invocation.
/// This is what the gateway serializes and returns to the client.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RagResponse {
    /// Echoes the originating query ID for correlation in the audit log.
    pub query_id: Uuid,
    /// LLM-generated answer grounded in the retrieved source documents.
    pub answer: String,
    /// The document chunks used to ground the answer, in ranked order.
    pub sources: Vec<RetrievedDocument>,
    /// Ollama model that generated the answer (e.g. "mistral-nemo").
    pub model: String,
    /// Wall-clock time for the full embed → retrieve → generate cycle (ms).
    pub latency_ms: u64,
}

// ---------------------------------------------------------------------------
// Error types
// ---------------------------------------------------------------------------

/// All errors that can propagate through the RAG pipeline.
#[derive(Debug, thiserror::Error)]
pub enum RagError {
    #[error("tokenization failed: {0}")]
    TokenizationFailed(String),

    #[error("embedding inference failed: {0}")]
    EmbeddingFailed(String),

    #[error("vector retrieval failed: {0}")]
    RetrievalFailed(String),

    #[error("LLM generation failed: {0}")]
    GenerationFailed(String),

    #[error("vault error: {0}")]
    VaultError(String),

    #[error("model not loaded — call Pipeline::load() before querying")]
    ModelNotLoaded,

    #[error("index is empty — ingest documents before querying")]
    IndexEmpty,

    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
}

/// Convenience alias used throughout all ra-rag modules.
pub type RagResult<T> = Result<T, RagError>;
