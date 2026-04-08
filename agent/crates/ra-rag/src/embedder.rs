// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/embedder.rs
// MISSION: ONNX Runtime embedding engine.
//          Model:   Alibaba-NLP/gte-large-en-v1.5 (1024-dim, 8192-ctx, clean ONNX)
//          Runtime: ort v2 RC with CUDAExecutionProvider (RTX 3060, device 0)
//          Pooling: masked mean pooling over last_hidden_state → L2 normalize
//
// PHASE 2 NOTE: When the fabric mesh is active, embed_batch() will dispatch
//               jobs to lvm_nodes via CMD_EMBED instead of running inline.
//               The public interface (embed / embed_batch) does not change.

use std::path::Path;

use ndarray::{Array2, s};
use ort::{
    execution_providers::CUDAExecutionProvider,
    inputs,
    session::{builder::GraphOptimizationLevel, Session},
    value::Tensor,
};
use tokenizers::{
    PaddingDirection, PaddingParams, PaddingStrategy, Tokenizer, TruncationDirection,
    TruncationParams, TruncationStrategy,
};

use crate::types::{Embedding, RagError, RagResult};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

pub const MODEL_ID:      &str   = "Alibaba-NLP/gte-large-en-v1.5";
pub const EMBEDDING_DIM: usize  = 1024;
pub const MAX_SEQ_LEN:   usize  = 8192;

// ---------------------------------------------------------------------------
// Embedder
// ---------------------------------------------------------------------------

pub struct Embedder {
    session:   Session,
    tokenizer: Tokenizer,
}

impl Embedder {
    /// Load ONNX session + tokenizer from local paths.
    /// Falls back to CPU automatically if no CUDA device is available.
    pub fn load(onnx_path: &Path, tokenizer_path: &Path) -> RagResult<Self> {
        let session = Session::builder()
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?
            .with_optimization_level(GraphOptimizationLevel::Level3)
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?
            .with_execution_providers([CUDAExecutionProvider::default()
                .with_device_id(0)
                .build()])
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?
            .commit_from_file(onnx_path)
            .map_err(|e| RagError::EmbeddingFailed(
                format!("ONNX load failed ({}): {e}", onnx_path.display())
            ))?;

        tracing::info!(model = MODEL_ID, path = %onnx_path.display(), "ONNX session ready");

        let mut tokenizer = Tokenizer::from_file(tokenizer_path)
            .map_err(|e| RagError::TokenizationFailed(e.to_string()))?;

        tokenizer.with_padding(Some(PaddingParams {
            strategy:  PaddingStrategy::BatchLongest,
            direction: PaddingDirection::Right,
            pad_id:    0,
            pad_token: "[PAD]".to_string(),
            ..Default::default()
        }));

        tokenizer
            .with_truncation(Some(TruncationParams {
                max_length: MAX_SEQ_LEN,
                strategy:   TruncationStrategy::LongestFirst,
                direction:  TruncationDirection::Right,
                stride:     0,
            }))
            .map_err(|e| RagError::TokenizationFailed(e.to_string()))?;

        Ok(Self { session, tokenizer })
    }

    /// Embed a single text string.
    pub fn embed(&mut self, text: &str) -> RagResult<Embedding> {
        self.embed_batch(&[text]).map(|mut v| v.remove(0))
    }

    /// Embed a batch in one ONNX inference call.
    pub fn embed_batch(&mut self, texts: &[&str]) -> RagResult<Vec<Embedding>> {
        if texts.is_empty() {
            return Ok(vec![]);
        }

        // --- Tokenize ---
        let encodings = self
            .tokenizer
            .encode_batch(texts.to_vec(), true)
            .map_err(|e| RagError::TokenizationFailed(e.to_string()))?;

        let batch   = encodings.len();
        let seq_len = encodings[0].get_ids().len();

        let mut input_ids      = Array2::<i64>::zeros((batch, seq_len));
        let mut attention_mask = Array2::<i64>::zeros((batch, seq_len));
        let mut token_type_ids = Array2::<i64>::zeros((batch, seq_len));

        for (i, enc) in encodings.iter().enumerate() {
            for (j, &id)   in enc.get_ids().iter().enumerate()            { input_ids[[i,j]]      = id as i64; }
            for (j, &mask) in enc.get_attention_mask().iter().enumerate() { attention_mask[[i,j]] = mask as i64; }
            for (j, &tid)  in enc.get_type_ids().iter().enumerate()       { token_type_ids[[i,j]] = tid as i64; }
        }

        // --- Build ort Tensor values from ndarray (ndarray feature) ---
        let ids_val  = Tensor::from_array(input_ids.clone())
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;
        let mask_val = Tensor::from_array(attention_mask.clone())
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;
        let type_val = Tensor::from_array(token_type_ids)
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;

        // --- Inference ---
        let outputs = self
            .session
            .run(inputs! {
                "input_ids"      => ids_val,
                "attention_mask" => mask_val,
                "token_type_ids" => type_val,
            })
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;

        // --- Extract last_hidden_state as ArrayViewD<f32> (ndarray feature) ---
        let hidden_view = outputs["last_hidden_state"]
            .try_extract_array::<f32>()
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;

        // Reshape IxDyn → [batch, seq_len, hidden_dim]
        let hidden = hidden_view
            .into_dimensionality::<ndarray::Ix3>()
            .map_err(|e| RagError::EmbeddingFailed(e.to_string()))?;

        // --- Masked mean pool + L2 normalize ---
        let mask_f32 = attention_mask.mapv(|x| x as f32);
        let vectors  = mean_pool_normalize(&hidden.to_owned(), &mask_f32);

        Ok(vectors
            .into_iter()
            .map(|v| Embedding::new(v, MODEL_ID))
            .collect())
    }
}

// ---------------------------------------------------------------------------
// Pooling (pure Rust — runs on CPU output tensors)
// ---------------------------------------------------------------------------

fn mean_pool_normalize(
    hidden: &ndarray::Array3<f32>,
    mask:   &Array2<f32>,
) -> Vec<Vec<f32>> {
    let (batch, _seq_len, hidden_dim) = hidden.dim();
    let mut result = Vec::with_capacity(batch);

    for b in 0..batch {
        let h = hidden.slice(s![b, .., ..]); // [seq_len, hidden_dim]
        let m = mask.slice(s![b, ..]);        // [seq_len]

        let mut sum         = vec![0f32; hidden_dim];
        let mut token_count = 0f32;

        for (t, &mask_val) in m.iter().enumerate() {
            if mask_val > 0.0 {
                token_count += 1.0;
                for d in 0..hidden_dim {
                    sum[d] += h[[t, d]];
                }
            }
        }

        let denom = token_count.max(1e-9);
        for v in &mut sum { *v /= denom; }

        let norm: f32 = sum.iter().map(|x| x * x).sum::<f32>().sqrt().max(1e-12);
        for v in &mut sum { *v /= norm; }

        result.push(sum);
    }

    result
}
