use axum::{extract::{Path, Query, State}, http::Method, routing::get, Json, Router};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tower_http::cors::{Any, CorsLayer};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecommendedProduct {
    pub id: u32,
    pub name: String,
    pub price: f64,
    pub rating: f64,
    pub image_url: String,
    pub reason: String,
}

#[derive(Deserialize)]
pub struct RecoQuery {
    pub limit: Option<u32>,
}

// Precomputed affinity map: product_id -> list of related product_ids
type AffinityMap = Arc<Vec<Vec<u32>>>;

fn build_affinity() -> Vec<Vec<u32>> {
    // For each product, related = same category bucket neighbors
    (0usize..500).map(|i| {
        let base = (i / 10) * 10;
        (base..base + 10)
            .filter(|&j| j != i && j < 500)
            .map(|j| (j + 1) as u32)
            .take(8)
            .collect()
    }).collect()
}

fn product_name(id: u32) -> String {
    let brands = ["SovereignTech", "NexaCore", "PrimeForge", "QuantumLeap", "ApexEdge"];
    let subs = ["Laptop", "Smartphone", "Headset", "Watch", "Camera", "Tablet", "Speaker", "Monitor"];
    format!("{} {} {}", brands[((id-1) % 5) as usize], subs[((id-1) % 8) as usize], id)
}

async fn related(
    State(db): State<AffinityMap>,
    Path(product_id): Path<u32>,
    Query(q): Query<RecoQuery>,
) -> Json<Vec<RecommendedProduct>> {
    let limit = q.limit.unwrap_or(8).min(16) as usize;
    let idx = (product_id.saturating_sub(1)) as usize;
    let related_ids = db.get(idx).cloned().unwrap_or_default();
    let result = related_ids.into_iter().take(limit).map(|id| {
        let price = (29.99 + (id as f64 * 2.13) % 1500.0);
        let price = (price * 100.0).round() / 100.0;
        RecommendedProduct {
            id,
            name: product_name(id),
            price,
            rating: (3.5 + ((id * 11) % 15) as f64 / 10.0).min(5.0),
            image_url: format!("https://picsum.photos/seed/{}/400/300", id),
            reason: "Customers also bought".to_string(),
        }
    }).collect();
    Json(result)
}

async fn trending(State(db): State<AffinityMap>) -> Json<Vec<RecommendedProduct>> {
    let ids: Vec<u32> = vec![1,17,34,51,68,85,102,119,136,153,170,187];
    let result = ids.into_iter().map(|id| {
        let price = (49.99 + (id as f64 * 3.17) % 2000.0);
        let price = (price * 100.0).round() / 100.0;
        RecommendedProduct {
            id,
            name: product_name(id),
            price,
            rating: (4.0 + ((id * 13) % 10) as f64 / 10.0).min(5.0),
            image_url: format!("https://picsum.photos/seed/{}/400/300", id),
            reason: "Trending this week".to_string(),
        }
    }).collect();
    Json(result)
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8008".into()).parse().unwrap_or(8008);
    let db: AffinityMap = Arc::new(build_affinity());
    let cors = CorsLayer::new().allow_methods([Method::GET]).allow_origin(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/recommendations/{product_id}", get(related))
        .route("/trending", get(trending))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("recommendation-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
