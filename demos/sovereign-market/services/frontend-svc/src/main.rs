use axum::{routing::get_service, Router};
use std::net::SocketAddr;
use tower_http::{cors::{Any, CorsLayer}, services::ServeDir};
use axum::http::Method;

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "4200".into()).parse().unwrap_or(4200);
    let static_dir = std::env::var("STATIC_DIR").unwrap_or_else(|_| "./dist/sovereign-market/browser".into());

    let cors = CorsLayer::new().allow_methods([Method::GET]).allow_origin(Any);

    let app = Router::new()
        .nest_service("/", get_service(ServeDir::new(&static_dir).append_index_html_on_directories(true)))
        .layer(cors);

    let addr = format!("0.0.0.0:{}", port);
    tracing::info!("frontend-svc serving {} on {}", static_dir, addr);
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}
