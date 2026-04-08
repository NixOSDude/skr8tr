// SSoA LEVEL 2: MANIFOLD ANCHOR
// FILE: crates/ra-rag/src/generator.rs
// MISSION: LLM response generation via OpenAI-compatible chat completions API.
//          Supports llama.cpp server, Ollama (OpenAI-compat mode), and any
//          OpenAI-compatible endpoint (LM Studio, vLLM, etc.)
//
//          Two generation modes:
//            generate()          — General factual RAG (precise assistant role)
//            generate_lambdac()  — LambdaC code generation (LambAssist role)
//
// DEFAULT MODEL:    mistral-nemo (Mistral-NeMo-12B — sovereign local inference)
// DEFAULT BASE URL: http://localhost:8080  (llama.cpp server default port)
//                   Override via RA_LLM_BASE_URL env var.
//
// OLLAMA COMPAT: Set RA_LLM_BASE_URL=http://localhost:11434 (still uses
//                /v1/chat/completions — Ollama supports this endpoint).

use reqwest::Client;
use serde::{Deserialize, Serialize};

use crate::types::{RagError, RagResult, RetrievedDocument};

// ---------------------------------------------------------------------------
// OpenAI-compatible wire types
// ---------------------------------------------------------------------------

#[derive(Serialize)]
struct ChatRequest<'a> {
    model:       &'a str,
    messages:    Vec<ChatMessage>,
    temperature: f32,
    max_tokens:  i32,
    stream:      bool,
}

#[derive(Serialize, Clone)]
struct ChatMessage {
    role:    String,
    content: String,
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Vec<ChatChoice>,
    #[serde(default)]
    model: String,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: ChatResponseMessage,
}

#[derive(Deserialize)]
struct ChatResponseMessage {
    content: String,
}

// ---------------------------------------------------------------------------
// Prompt builders
// ---------------------------------------------------------------------------

/// Maximum characters of source content to include per retrieved document.
const MAX_CHARS_PER_SOURCE: usize = 1_500;

/// Maximum number of source documents to include in the prompt context.
const MAX_SOURCES_IN_PROMPT: usize = 8;

/// General-purpose RAG prompt — factual assistant grounded in retrieved sources.
/// Returns (system_message, user_message).
fn build_rag_messages(question: &str, sources: &[RetrievedDocument]) -> (String, String) {
    let system = "You are a precise, factual assistant. \
                  Answer the question using ONLY the provided source documents. \
                  If the answer is not contained in the sources, say \
                  \"I don't have enough information to answer that.\""
        .to_string();

    let mut user = String::with_capacity(8_192);
    user.push_str("--- SOURCE DOCUMENTS ---\n");
    for doc in sources.iter().take(MAX_SOURCES_IN_PROMPT) {
        let content = &doc.document.content[..doc.document.content.len().min(MAX_CHARS_PER_SOURCE)];
        user.push_str(&format!(
            "\n[Source {} | {} | score={:.3}]\n{}\n",
            doc.rank, doc.document.metadata.source, doc.score, content
        ));
    }
    user.push_str("\n--- END SOURCES ---\n\n");
    user.push_str(&format!("Question: {}", question));

    (system, user)
}

/// LambAssist prompt — specialized for LambdaC code generation.
/// Returns (system_message, user_message).
fn build_lambdac_messages(
    user_prompt: &str,
    schema: &str,
    sources: &[RetrievedDocument],
) -> (String, String) {
    let system = "\
You are LambAssist, the sovereign AI interface for LambData — a GPU-native distributed data platform.
Your role: convert natural language data questions into valid LambdaC code.

RULES:
1. Generate LambdaC code wrapped in ```lambdac ... ``` blocks.
2. Use ONLY the operations documented in the reference sources below.
3. Use ONLY the column names from the provided schema.
4. Start with load_ldb() for data already in LambBook, or load_csv/load_parquet for files.
5. Chain operations with let...in bindings.
6. After the code block, briefly explain the generated code in plain English."
        .to_string();

    let mut user = String::with_capacity(12_288);

    // Schema context from LambBook's live frame state
    if !schema.is_empty() && schema != "no frames loaded" {
        user.push_str("--- CURRENT DATA SCHEMA (frames loaded in LambBook) ---\n");
        user.push_str(schema);
        user.push_str("\n--- END SCHEMA ---\n\n");
    }

    // Retrieved LambdaC language and codebase references
    if !sources.is_empty() {
        user.push_str("--- LAMBDAC REFERENCE (language docs + codebase) ---\n");
        for doc in sources.iter().take(MAX_SOURCES_IN_PROMPT) {
            let content = &doc.document.content[..doc.document.content.len().min(MAX_CHARS_PER_SOURCE)];
            user.push_str(&format!(
                "\n[Ref {} | {} | relevance={:.3}]\n{}\n",
                doc.rank, doc.document.metadata.source, doc.score, content
            ));
        }
        user.push_str("\n--- END REFERENCE ---\n\n");
    }

    user.push_str(&format!("User request: {}", user_prompt));

    (system, user)
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

/// Stateless async LLM client.  `Clone` freely — reqwest::Client is Arc-backed.
#[derive(Clone)]
pub struct Generator {
    client:   Client,
    base_url: String,
    model:    String,
}

impl Generator {
    pub const DEFAULT_MODEL: &'static str = "mistral-nemo";

    /// Create a generator pointing at an OpenAI-compatible inference server.
    ///
    /// Base URL resolution order:
    ///   1. `RA_LLM_BASE_URL` env var
    ///   2. `OLLAMA_BASE_URL` env var  (backward compat)
    ///   3. Default: http://localhost:8080  (llama.cpp server)
    pub fn new(model: impl Into<String>) -> Self {
        let base_url = std::env::var("RA_LLM_BASE_URL")
            .or_else(|_| std::env::var("OLLAMA_BASE_URL"))
            .unwrap_or_else(|_| "http://localhost:8080".to_string());
        Self {
            client:   Client::new(),
            base_url,
            model:    model.into(),
        }
    }

    pub fn with_default_model() -> Self {
        Self::new(Self::DEFAULT_MODEL)
    }

    /// Generate a grounded answer from retrieved source documents.
    /// Uses the factual assistant persona — for general RAG queries.
    pub async fn generate(
        &self,
        question: &str,
        sources: &[RetrievedDocument],
    ) -> RagResult<String> {
        let (system, user) = build_rag_messages(question, sources);
        self.call_chat(system, user, 1_024, 0.1).await
    }

    /// Generate a LambdaC code response grounded in language docs + live schema.
    /// Uses the LambAssist persona — always produces ```lambdac code blocks.
    pub async fn generate_lambdac(
        &self,
        user_prompt: &str,
        schema: &str,
        sources: &[RetrievedDocument],
    ) -> RagResult<String> {
        let (system, user) = build_lambdac_messages(user_prompt, schema, sources);
        // Temperature 0.2: low enough for code accuracy, slight creativity for novel queries.
        self.call_chat(system, user, 2_048, 0.2).await
    }

    /// Shared OpenAI-compatible chat/completions invocation.
    async fn call_chat(
        &self,
        system_msg: String,
        user_msg:   String,
        max_tokens: i32,
        temperature: f32,
    ) -> RagResult<String> {
        let request = ChatRequest {
            model:       &self.model,
            messages:    vec![
                ChatMessage { role: "system".to_string(), content: system_msg },
                ChatMessage { role: "user".to_string(),   content: user_msg  },
            ],
            temperature,
            max_tokens,
            stream: false,
        };

        let url = format!("{}/v1/chat/completions", self.base_url);
        tracing::debug!(url = %url, model = %self.model, "sending chat request");

        let resp = self
            .client
            .post(&url)
            .json(&request)
            .send()
            .await
            .map_err(|e| RagError::GenerationFailed(format!("LLM request failed: {e}")))?;

        if !resp.status().is_success() {
            let status = resp.status();
            let body   = resp.text().await.unwrap_or_default();
            return Err(RagError::GenerationFailed(format!(
                "LLM returned HTTP {status}: {body}"
            )));
        }

        let body: ChatResponse = resp
            .json()
            .await
            .map_err(|e| RagError::GenerationFailed(format!("failed to parse LLM response: {e}")))?;

        let content = body
            .choices
            .into_iter()
            .next()
            .map(|c| c.message.content)
            .unwrap_or_default();

        tracing::debug!(
            model = %self.model,
            chars = content.len(),
            "generation complete"
        );

        Ok(content.trim().to_string())
    }

    pub fn model(&self) -> &str {
        &self.model
    }
}
