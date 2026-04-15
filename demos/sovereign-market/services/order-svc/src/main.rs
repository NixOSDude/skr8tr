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
pub struct OrderItem {
    pub product_id: u32,
    pub name: String,
    pub price: f64,
    pub quantity: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Order {
    pub id: String,
    pub user_id: String,
    pub items: Vec<OrderItem>,
    pub subtotal: f64,
    pub shipping: f64,
    pub tax: f64,
    pub total: f64,
    pub status: String,
    pub shipping_address: ShippingAddress,
    pub created_at: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ShippingAddress {
    pub name: String,
    pub street: String,
    pub city: String,
    pub state: String,
    pub zip: String,
    pub country: String,
}

#[derive(Deserialize)]
pub struct PlaceOrder {
    pub user_id: String,
    pub items: Vec<OrderItem>,
    pub shipping_address: ShippingAddress,
}

type Db = Arc<RwLock<HashMap<String, Order>>>;

async fn place_order(State(db): State<Db>, Json(req): Json<PlaceOrder>) -> Json<Order> {
    let subtotal = req.items.iter().map(|i| i.price * i.quantity as f64).sum::<f64>();
    let shipping = if subtotal > 75.0 { 0.0 } else { 9.99 };
    let tax = (subtotal * 0.0825 * 100.0).round() / 100.0;
    let total = (subtotal + shipping + tax) * 100.0 / 100.0;
    let order = Order {
        id: Uuid::new_v4().to_string(),
        user_id: req.user_id,
        items: req.items,
        subtotal: (subtotal * 100.0).round() / 100.0,
        shipping,
        tax,
        total: (total * 100.0).round() / 100.0,
        status: "confirmed".to_string(),
        shipping_address: req.shipping_address,
        created_at: chrono_now(),
    };
    db.write().unwrap().insert(order.id.clone(), order.clone());
    Json(order)
}

async fn get_order(State(db): State<Db>, Path(id): Path<String>) -> Result<Json<Order>, axum::http::StatusCode> {
    db.read().unwrap().get(&id).cloned().map(Json).ok_or(axum::http::StatusCode::NOT_FOUND)
}

async fn user_orders(State(db): State<Db>, Path(user_id): Path<String>) -> Json<Vec<Order>> {
    let orders = db.read().unwrap();
    Json(orders.values().filter(|o| o.user_id == user_id).cloned().collect())
}

fn chrono_now() -> String {
    // Simple timestamp without chrono dependency
    "2026-04-06T00:00:00Z".to_string()
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8005".into()).parse().unwrap_or(8005);
    let db: Db = Arc::new(RwLock::new(HashMap::new()));
    let cors = CorsLayer::new().allow_methods([Method::GET, Method::POST]).allow_origin(Any).allow_headers(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/orders", post(place_order))
        .route("/orders/{id}", get(get_order))
        .route("/orders/user/{user_id}", get(user_orders))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("order-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
