use axum::{
    extract::{Path, State},
    http::Method,
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, sync::{Arc, RwLock}};
use tower_http::cors::{Any, CorsLayer};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Review {
    pub id: String,
    pub product_id: u32,
    pub user_id: String,
    pub user_name: String,
    pub rating: u8,
    pub title: String,
    pub body: String,
    pub verified: bool,
    pub created_at: String,
}

#[derive(Deserialize)]
pub struct SubmitReview {
    pub user_id: String,
    pub user_name: String,
    pub rating: u8,
    pub title: String,
    pub body: String,
}

#[derive(Serialize)]
pub struct ReviewSummary {
    pub product_id: u32,
    pub average_rating: f64,
    pub total_reviews: u32,
    pub distribution: HashMap<u8, u32>,
    pub reviews: Vec<Review>,
}

type Db = Arc<RwLock<HashMap<u32, Vec<Review>>>>;

fn seed_reviews() -> HashMap<u32, Vec<Review>> {
    let mut map: HashMap<u32, Vec<Review>> = HashMap::new();
    let names = ["Alice K.", "Bob M.", "Carol T.", "David R.", "Eva S.", "Frank L.", "Grace H.", "Henry P."];
    let titles = ["Excellent product", "Great value", "Highly recommended", "Good but not perfect", "Amazing quality", "Worth every penny", "Solid build", "Impressive performance"];
    let bodies = [
        "Exactly what I needed. Works perfectly out of the box.",
        "Great product for the price. Would buy again.",
        "Top quality materials and excellent craftsmanship.",
        "Does what it says. A few minor quirks but nothing major.",
        "Blown away by the quality. This exceeded my expectations.",
        "The build quality is outstanding. Very happy with this purchase.",
        "Solid and reliable. Has been working great for months.",
        "Fast shipping, great packaging, and the product is perfect.",
    ];
    for pid in 1u32..=500 {
        let count = ((pid * 7 + 13) % 12) as usize;
        let mut reviews = Vec::new();
        for j in 0..count {
            let idx = (pid as usize + j) % 8;
            reviews.push(Review {
                id: format!("rev-{}-{}", pid, j),
                product_id: pid,
                user_id: format!("user-{}", idx),
                user_name: names[idx].to_string(),
                rating: (((pid + j as u32) * 3 + 2) % 5 + 1) as u8,
                title: titles[idx].to_string(),
                body: bodies[idx].to_string(),
                verified: j % 2 == 0,
                created_at: "2026-04-06T00:00:00Z".to_string(),
            });
        }
        if !reviews.is_empty() {
            map.insert(pid, reviews);
        }
    }
    map
}

async fn get_reviews(State(db): State<Db>, Path(product_id): Path<u32>) -> Json<ReviewSummary> {
    let store = db.read().unwrap();
    let reviews = store.get(&product_id).cloned().unwrap_or_default();
    let total = reviews.len() as u32;
    let avg = if total > 0 { reviews.iter().map(|r| r.rating as f64).sum::<f64>() / total as f64 } else { 0.0 };
    let avg = (avg * 10.0).round() / 10.0;
    let mut dist: HashMap<u8, u32> = HashMap::new();
    for r in &reviews { *dist.entry(r.rating).or_default() += 1; }
    Json(ReviewSummary { product_id, average_rating: avg, total_reviews: total, distribution: dist, reviews })
}

async fn submit_review(State(db): State<Db>, Path(product_id): Path<u32>, Json(req): Json<SubmitReview>) -> Json<Review> {
    let review = Review {
        id: Uuid::new_v4().to_string(),
        product_id,
        user_id: req.user_id,
        user_name: req.user_name,
        rating: req.rating.clamp(1, 5),
        title: req.title,
        body: req.body,
        verified: false,
        created_at: "2026-04-06T00:00:00Z".to_string(),
    };
    db.write().unwrap().entry(product_id).or_default().push(review.clone());
    Json(review)
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8007".into()).parse().unwrap_or(8007);
    let db: Db = Arc::new(RwLock::new(seed_reviews()));
    let cors = CorsLayer::new().allow_methods([Method::GET, Method::POST]).allow_origin(Any).allow_headers(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/reviews/{product_id}", get(get_reviews))
        .route("/reviews/{product_id}", post(submit_review))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("review-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
