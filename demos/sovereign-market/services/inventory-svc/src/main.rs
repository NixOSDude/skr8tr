use axum::{extract::{Path, State}, http::Method, routing::get, Json, Router};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, sync::Arc};
use tower_http::cors::{Any, CorsLayer};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StockEntry {
    pub product_id: u32,
    pub sku: String,
    pub quantity: u32,
    pub reserved: u32,
    pub available: u32,
    pub warehouse: String,
    pub reorder_point: u32,
    pub status: String,
}

type Db = Arc<HashMap<u32, StockEntry>>;

fn generate_inventory() -> HashMap<u32, StockEntry> {
    let warehouses = ["West-Coast-DC", "East-Coast-DC", "Central-Hub", "EU-Frankfurt"];
    let mut map = HashMap::new();
    for id in 1u32..=500 {
        let quantity = if id % 11 == 0 { 0 } else { (id * 7 + 43) % 500 };
        let reserved = if quantity > 0 { (id * 3) % quantity.max(1) } else { 0 };
        let available = quantity.saturating_sub(reserved);
        let status = match available {
            0 => "out_of_stock",
            1..=5 => "low_stock",
            _ => "in_stock",
        }.to_string();
        map.insert(id, StockEntry {
            product_id: id,
            sku: format!("SKU-{:05}", id),
            quantity,
            reserved,
            available,
            warehouse: warehouses[((id - 1) % 4) as usize].to_string(),
            reorder_point: 20,
            status,
        });
    }
    map
}

async fn get_stock(State(db): State<Db>, Path(id): Path<u32>) -> Result<Json<StockEntry>, axum::http::StatusCode> {
    db.get(&id).cloned().map(Json).ok_or(axum::http::StatusCode::NOT_FOUND)
}

async fn bulk_stock(State(db): State<Db>, Json(ids): Json<Vec<u32>>) -> Json<Vec<StockEntry>> {
    Json(ids.iter().filter_map(|id| db.get(id).cloned()).collect())
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8002".into()).parse().unwrap_or(8002);
    let db: Db = Arc::new(generate_inventory());
    let cors = CorsLayer::new().allow_methods([Method::GET, Method::POST]).allow_origin(Any).allow_headers(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/stock/{id}", get(get_stock))
        .route("/stock/bulk", axum::routing::post(bulk_stock))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("inventory-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
