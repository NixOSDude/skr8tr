use axum::{
    extract::{Path, State},
    http::Method,
    routing::{get, post, put},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, sync::{Arc, RwLock}};
use tower_http::cors::{Any, CorsLayer};
use uuid::Uuid;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct User {
    pub id: String,
    pub email: String,
    pub name: String,
    pub role: String,
    pub created_at: String,
}

#[derive(Deserialize)]
pub struct RegisterUser {
    pub email: String,
    pub name: String,
    pub password: String,
}

#[derive(Deserialize)]
pub struct LoginUser {
    pub email: String,
    pub password: String,
}

#[derive(Serialize)]
pub struct AuthResponse {
    pub user: User,
    pub token: String,
}

type Db = Arc<RwLock<HashMap<String, User>>>;

async fn register(State(db): State<Db>, Json(req): Json<RegisterUser>) -> Json<AuthResponse> {
    let user = User {
        id: Uuid::new_v4().to_string(),
        email: req.email.clone(),
        name: req.name,
        role: "user".to_string(),
        created_at: "2026-04-06T00:00:00Z".to_string(),
    };
    db.write().unwrap().insert(req.email.clone(), user.clone());
    let token = format!("tok-{}", Uuid::new_v4());
    Json(AuthResponse { user, token })
}

async fn login(State(db): State<Db>, Json(req): Json<LoginUser>) -> Result<Json<AuthResponse>, axum::http::StatusCode> {
    let store = db.read().unwrap();
    if let Some(user) = store.get(&req.email) {
        let token = format!("tok-{}", Uuid::new_v4());
        Ok(Json(AuthResponse { user: user.clone(), token }))
    } else {
        Err(axum::http::StatusCode::UNAUTHORIZED)
    }
}

async fn get_user(State(db): State<Db>, Path(id): Path<String>) -> Result<Json<User>, axum::http::StatusCode> {
    let store = db.read().unwrap();
    store.values().find(|u| u.id == id).cloned().map(Json).ok_or(axum::http::StatusCode::NOT_FOUND)
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8006".into()).parse().unwrap_or(8006);
    let db: Db = Arc::new(RwLock::new(HashMap::new()));
    let cors = CorsLayer::new().allow_methods([Method::GET, Method::POST, Method::PUT]).allow_origin(Any).allow_headers(Any);
    let app = Router::new()
        .route("/health", get(health))
        .route("/register", post(register))
        .route("/login", post(login))
        .route("/users/{id}", get(get_user))
        .layer(cors)
        .with_state(db);
    let listener = tokio::net::TcpListener::bind(format!("0.0.0.0:{}", port)).await.unwrap();
    tracing::info!("user-svc on :{}", port);
    axum::serve(listener, app).await.unwrap();
}
