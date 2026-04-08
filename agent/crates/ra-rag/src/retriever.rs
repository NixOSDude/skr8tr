// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/retriever.rs
// MISSION: usearch HNSW vector store — index management, similarity search,
//          full doc_store persistence (snapshot + restore).

use std::{
    collections::HashMap,
    path::Path,
};

use usearch::{Index, IndexOptions, MetricKind, ScalarKind};

use crate::types::{Document, Embedding, RagError, RagResult, RetrievedDocument};

const HNSW_CONNECTIVITY:      usize = 16;
const HNSW_EXPANSION_ADD:     usize = 128;
const HNSW_EXPANSION_SEARCH:  usize = 64;
const INITIAL_CAPACITY:       usize = 131_072;

pub struct Retriever {
    index:     Index,
    doc_store: HashMap<u64, Document>,
    next_key:  u64,
    dimension: usize,
}

impl Retriever {
    pub fn new(dimension: usize) -> RagResult<Self> {
        let options = IndexOptions {
            dimensions:       dimension,
            metric:           MetricKind::Cos,
            quantization:     ScalarKind::F32,
            connectivity:     HNSW_CONNECTIVITY,
            expansion_add:    HNSW_EXPANSION_ADD,
            expansion_search: HNSW_EXPANSION_SEARCH,
            ..Default::default()
        };
        let index = Index::new(&options)
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;
        index.reserve(INITIAL_CAPACITY)
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;
        tracing::info!(dimension, "usearch HNSW index created");
        Ok(Self { index, doc_store: HashMap::new(), next_key: 0, dimension })
    }

    pub fn add(&mut self, doc: Document, embedding: &Embedding) -> RagResult<()> {
        debug_assert_eq!(embedding.dimension, self.dimension);
        let key = self.next_key;
        self.next_key += 1;
        self.index.add(key, &embedding.vector)
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;
        self.doc_store.insert(key, doc);
        Ok(())
    }

    pub fn search(
        &self,
        query_embedding: &Embedding,
        top_k: usize,
    ) -> RagResult<Vec<RetrievedDocument>> {
        if self.is_empty() {
            return Err(RagError::IndexEmpty);
        }
        let k = top_k.min(self.len());
        let matches = self.index.search(&query_embedding.vector, k)
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;

        let mut retrieved = Vec::with_capacity(matches.keys.len());
        for (rank, (&key, &distance)) in matches.keys.iter()
            .zip(matches.distances.iter()).enumerate()
        {
            if let Some(doc) = self.doc_store.get(&key) {
                retrieved.push(RetrievedDocument {
                    document: doc.clone(),
                    score: 1.0 - distance,
                    rank: (rank + 1) as u32,
                });
            }
        }
        Ok(retrieved)
    }

    /// Save HNSW graph to disk.
    pub fn save_index(&self, path: &Path) -> RagResult<()> {
        self.index.save(path.to_str().unwrap_or("index.usearch"))
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;
        tracing::info!(path = %path.display(), size = self.len(), "HNSW index saved");
        Ok(())
    }

    /// Load a previously saved HNSW graph. doc_store must be restored separately.
    pub fn load_index(path: &Path, dimension: usize) -> RagResult<Self> {
        let options = IndexOptions {
            dimensions:       dimension,
            metric:           MetricKind::Cos,
            quantization:     ScalarKind::F32,
            connectivity:     HNSW_CONNECTIVITY,
            expansion_add:    HNSW_EXPANSION_ADD,
            expansion_search: HNSW_EXPANSION_SEARCH,
            ..Default::default()
        };
        let index = Index::new(&options)
            .map_err(|e| RagError::RetrievalFailed(e.to_string()))?;
        index.load(path.to_str().unwrap_or("index.usearch"))
            .map_err(|e| RagError::RetrievalFailed(format!(
                "failed to load index from {}: {e}", path.display()
            )))?;
        let next_key = index.size() as u64;
        tracing::info!(path = %path.display(), size = next_key, "HNSW index loaded");
        Ok(Self { index, doc_store: HashMap::new(), next_key, dimension })
    }

    /// Restore doc_store from a persisted snapshot.
    pub fn restore_doc_store(&mut self, docs: impl IntoIterator<Item = (u64, Document)>) {
        for (key, doc) in docs {
            self.doc_store.insert(key, doc);
        }
    }

    /// Export doc_store as a snapshot for persistence.
    /// Returns all (key, Document) pairs sorted by key for deterministic output.
    pub fn snapshot_doc_store(&self) -> Vec<(u64, Document)> {
        let mut entries: Vec<(u64, Document)> = self.doc_store
            .iter()
            .map(|(&k, v)| (k, v.clone()))
            .collect();
        entries.sort_by_key(|(k, _)| *k);
        entries
    }

    pub fn len(&self) -> usize       { self.index.size() }
    pub fn is_empty(&self) -> bool   { self.index.size() == 0 }
    pub fn dimension(&self) -> usize { self.dimension }
}
