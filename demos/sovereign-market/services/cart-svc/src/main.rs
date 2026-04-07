use axum::{
    extract::{Path, State},
    http::Method,
    routing::{delete, get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, sync::{Arc, RwLock}};
use tower_http::cors::{Any, CorsLayer};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CartItem {
    pub product_id: u32,
    pub name: String,
    pub price: f64,
    pub quantity: u32,
    pub image_url: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Cart {
    pub session_id: String,
    pub items: Vec<CartItem>,
    pub subtotal: f64,
    pub item_count: u32,
}

#[derive(Deserialize)]
pub struct AddItem {
    pub product_id: u32,
    pub name: String,
    pub price: f64,
    pub quantity: u32,
    pub image_url: String,
}

#[derive(Deserialize)]
pub struct UpdateQuantity {
    pub quantity: u32,
}

type Db = Arc<RwLock<HashMap<String, Vec<CartItem>>>>;

fn compute_cart(session_id: String, items: Vec<CartItem>) -> Cart {
    let subtotal = items.iter().map(|i| i.price * i.quantity as f64).sum::<f64>();
    let subtotal = (subtotal * 100.0).round() / 100.0;
    let item_count = items.iter().map(|i| i.quantity).sum();
    Cart { session_id, items, subtotal, item_count }
}

async fn get_cart(State(db): State<Db>, Path(session): Path<String>) -> Json<Cart> {
    let items = db.read().unwrap().get(&session).cloned().unwrap_or_default();
    Json(compute_cart(session, items))
}

async fn add_item(State(db): State<Db>, Path(session): Path<String>, Json(item): Json<AddItem>) -> Json<Cart> {
    let mut store = db.write().unwrap();
    let items = store.entry(session.clone()).or_default();
    if let Some(existing) = items.iter_mut().find(|i| i.product_id == item.product_id) {
        existing.quantity += item.quantity;
    } else {
        items.push(CartItem { product_id: item.product_id, name: item.name, price: item.price, quantity: item.quantity, image_url: item.image_url });
    }
    let items = items.clone();
    drop(store);
    Json(compute_cart(session, items))
}

async fn remove_item(State(db): State<Db>, Path((session, product_id)): Path<(String, u32)>) -> Json<Cart> {
    let mut store = db.write().unwrap();
    let items = store.entry(session.clone()).or_default();
    items.retain(|i| i.product_id != product_id);
    let items = items.clone();
    drop(store);
    Json(compute_cart(session, items))
}

async fn clear_cart(State(db): State<Db>, Path(session): Path<String>) -> Json<Cart> {
    db.write().unwrap().remove(&session);
    Json(compute_cart(session, vec![]))
}

async fn new_session() -> Json<serde_json::Value> {
    Json(serde_json::json!({ "session_id": Uuid::new_v4().to_string() }))
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8004".into()).parse().unwrap_or(8004);
    let db: Db = Arc::new(RwLock::new(HashMap::new()));
    let cors = CorsLayer::new().allow_methods([Method::GET, Method::POST, Method::DELETE, Method::PUT]).allow_origin(Any).allow_headers(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/session", post(new_session))
        .route("/cart/{session}", get(get_cart))
        .route("/cart/{session}/items", post(add_item))
        .route("/cart/{session}/items/{product_id}", delete(remove_item))
        .route("/cart/{session}", delete(clear_cart))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("cart-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
