use axum::{extract::{Query, State}, http::Method, routing::get, Json, Router};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tower_http::cors::{Any, CorsLayer};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SearchProduct {
    pub id: u32,
    pub name: String,
    pub category: String,
    pub brand: String,
    pub price: f64,
    pub rating: f64,
    pub image_url: String,
}

#[derive(Deserialize)]
pub struct SearchQuery {
    pub q: String,
    pub page: Option<u32>,
    pub per_page: Option<u32>,
}

#[derive(Serialize)]
pub struct SearchResult {
    pub query: String,
    pub results: Vec<SearchProduct>,
    pub total: u32,
    pub page: u32,
    pub suggestions: Vec<String>,
}

type Db = Arc<Vec<SearchProduct>>;

fn build_index() -> Vec<SearchProduct> {
    let categories = ["Electronics", "Fashion", "Home & Garden", "Sports & Outdoors", "Books & Media", "Health & Beauty", "Automotive"];
    let brands = ["SovereignTech", "NexaCore", "PrimeForge", "QuantumLeap", "ApexEdge", "ZenithPro", "VelocityX", "OmniCraft", "TitanWorks", "NovaPulse"];
    let subcats = ["Laptops", "Smartphones", "Shoes", "Furniture", "Fitness", "Programming", "Skincare", "Car Electronics", "Cameras", "Watches", "Kitchen", "Running", "Vitamins", "Tablets", "Headphones"];
    (1u32..=500).map(|i| {
        let cat = categories[((i-1) / 72).min(6) as usize];
        let brand = brands[((i-1) % 10) as usize];
        let sub = subcats[((i-1) % 15) as usize];
        let price = (49.99 + (i as f64 * 2.71) % 2000.0);
        let price = (price * 100.0).round() / 100.0;
        SearchProduct {
            id: i,
            name: format!("{} {} {} {}", brand, sub, "Series", i),
            category: cat.to_string(),
            brand: brand.to_string(),
            price,
            rating: (3.5 + ((i * 13) % 15) as f64 / 10.0).min(5.0),
            image_url: format!("https://picsum.photos/seed/{}/400/300", i),
        }
    }).collect()
}

async fn search(State(db): State<Db>, Query(q): Query<SearchQuery>) -> Json<SearchResult> {
    let query = q.q.to_lowercase();
    let page = q.page.unwrap_or(1).max(1);
    let per_page = q.per_page.unwrap_or(20).clamp(1, 100);

    let matched: Vec<&SearchProduct> = db.iter().filter(|p| {
        p.name.to_lowercase().contains(&query)
            || p.category.to_lowercase().contains(&query)
            || p.brand.to_lowercase().contains(&query)
    }).collect();

    let total = matched.len() as u32;
    let offset = ((page - 1) * per_page) as usize;
    let results: Vec<SearchProduct> = matched.into_iter().skip(offset).take(per_page as usize).cloned().collect();

    let suggestions = if results.is_empty() {
        vec!["electronics".into(), "laptops".into(), "smartphones".into(), "fashion".into()]
    } else {
        vec![]
    };

    Json(SearchResult { query: q.q, results, total, page, suggestions })
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8003".into()).parse().unwrap_or(8003);
    let db: Db = Arc::new(build_index());
    let cors = CorsLayer::new().allow_methods([Method::GET]).allow_origin(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/search", get(search))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("search-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
