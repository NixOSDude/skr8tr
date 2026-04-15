use axum::{
    extract::{Path, Query, State},
    http::Method,
    routing::get,
    Json, Router,
};
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tower_http::cors::{Any, CorsLayer};
use tracing_subscriber;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Product {
    pub id: u32,
    pub sku: String,
    pub name: String,
    pub description: String,
    pub price: f64,
    pub category: String,
    pub subcategory: String,
    pub brand: String,
    pub rating: f64,
    pub review_count: u32,
    pub image_url: String,
    pub tags: Vec<String>,
    pub featured: bool,
    pub in_stock: bool,
}

#[derive(Deserialize)]
pub struct ProductQuery {
    pub page: Option<u32>,
    pub per_page: Option<u32>,
    pub category: Option<String>,
    pub min_price: Option<f64>,
    pub max_price: Option<f64>,
    pub brand: Option<String>,
    pub featured: Option<bool>,
    pub sort: Option<String>,
}

#[derive(Serialize)]
pub struct ProductPage {
    pub products: Vec<Product>,
    pub total: u32,
    pub page: u32,
    pub per_page: u32,
    pub total_pages: u32,
}

#[derive(Serialize)]
pub struct CategoryList {
    pub categories: Vec<Category>,
}

#[derive(Serialize)]
pub struct Category {
    pub name: String,
    pub subcategories: Vec<String>,
    pub count: u32,
}

type Db = Arc<Vec<Product>>;

fn generate_products() -> Vec<Product> {
    let categories = vec![
        ("Electronics", vec!["Laptops", "Smartphones", "Tablets", "Headphones", "Cameras", "Smart Home", "Gaming", "Wearables"]),
        ("Fashion", vec!["Men's Clothing", "Women's Clothing", "Shoes", "Accessories", "Watches", "Bags", "Jewelry", "Sports Wear"]),
        ("Home & Garden", vec!["Furniture", "Kitchen", "Bedding", "Lighting", "Tools", "Garden", "Storage", "Decor"]),
        ("Sports & Outdoors", vec!["Fitness", "Cycling", "Running", "Camping", "Water Sports", "Team Sports", "Yoga", "Climbing"]),
        ("Books & Media", vec!["Programming", "Business", "Science", "Fiction", "Self-Help", "History", "Art", "Music"]),
        ("Health & Beauty", vec!["Skincare", "Vitamins", "Fitness Supplements", "Hair Care", "Oral Care", "Fragrances", "Medical", "Baby"]),
        ("Automotive", vec!["Car Electronics", "Tools", "Parts", "Accessories", "Tires", "Lighting", "Interior", "Exterior"]),
    ];

    let brands = vec![
        "SovereignTech", "NexaCore", "PrimeForge", "QuantumLeap", "ApexEdge",
        "ZenithPro", "VelocityX", "OmniCraft", "TitanWorks", "NovaPulse",
        "EclipseGear", "StellarBuild", "CoreDrive", "IronClad", "LuminaX",
        "FluxSystems", "PeakPerform", "ArcForce", "NexusBrand", "UltraForm",
    ];

    let adjectives = vec![
        "Pro", "Elite", "Ultra", "Max", "Prime", "Sovereign", "Advanced",
        "Premium", "Turbo", "Apex", "Core", "Nexus", "Titan", "Vector",
        "Fusion", "Quantum", "Hyper", "Forge", "Zenith", "Pinnacle",
    ];

    let mut products = Vec::with_capacity(500);

    for i in 1u32..=500 {
        let cat_idx = ((i - 1) / 72) as usize;
        let cat_idx = cat_idx.min(categories.len() - 1);
        let (category, subcats) = &categories[cat_idx];
        let subcat_idx = ((i - 1) % subcats.len() as u32) as usize;
        let subcategory = subcats[subcat_idx];
        let brand = brands[((i - 1) % brands.len() as u32) as usize];
        let adj = adjectives[((i - 1) % adjectives.len() as u32) as usize];

        let base_price = match *category {
            "Electronics" => 49.99 + (i as f64 * 3.17) % 2500.0,
            "Fashion"     => 19.99 + (i as f64 * 1.43) % 450.0,
            "Home & Garden" => 24.99 + (i as f64 * 2.11) % 800.0,
            "Sports & Outdoors" => 14.99 + (i as f64 * 1.87) % 600.0,
            "Books & Media" => 9.99 + (i as f64 * 0.51) % 80.0,
            "Health & Beauty" => 12.99 + (i as f64 * 0.93) % 200.0,
            _ => 29.99 + (i as f64 * 1.61) % 1000.0,
        };

        let price = (base_price * 100.0).round() / 100.0;
        let rating = 3.0 + (((i * 17 + 31) % 200) as f64) / 100.0;
        let rating = (rating * 10.0).round() / 10.0;

        products.push(Product {
            id: i,
            sku: format!("SKU-{:05}", i),
            name: format!("{} {} {} {}", brand, adj, subcategory, i),
            description: format!(
                "The {} {} {} delivers enterprise-grade performance for demanding workloads. \
                 Engineered with precision components and rigorous quality control, \
                 this {} {} {}-class product is built for professionals who demand \
                 the best. Features advanced {} technology with a {} warranty and \
                 full compatibility with leading industry standards.",
                brand, adj, subcategory, category, brand, adj,
                subcategory, if i % 3 == 0 { "2-year" } else { "1-year" }
            ),
            price,
            category: category.to_string(),
            subcategory: subcategory.to_string(),
            brand: brand.to_string(),
            rating: rating.min(5.0),
            review_count: (i * 7 + 13) % 1200,
            image_url: format!("https://picsum.photos/seed/{}/400/300", i),
            tags: vec![
                subcategory.to_lowercase().replace(' ', "-"),
                brand.to_lowercase(),
                adj.to_lowercase(),
                category.to_lowercase().replace(' ', "-"),
            ],
            featured: i % 17 == 0,
            in_stock: i % 11 != 0,
        });
    }

    products
}

async fn list_products(
    State(db): State<Db>,
    Query(q): Query<ProductQuery>,
) -> Json<ProductPage> {
    let page = q.page.unwrap_or(1).max(1);
    let per_page = q.per_page.unwrap_or(24).clamp(1, 100);

    let mut filtered: Vec<&Product> = db.iter().collect();

    if let Some(cat) = &q.category {
        filtered.retain(|p| p.category.eq_ignore_ascii_case(cat) || p.subcategory.eq_ignore_ascii_case(cat));
    }
    if let Some(min) = q.min_price {
        filtered.retain(|p| p.price >= min);
    }
    if let Some(max) = q.max_price {
        filtered.retain(|p| p.price <= max);
    }
    if let Some(brand) = &q.brand {
        filtered.retain(|p| p.brand.eq_ignore_ascii_case(brand));
    }
    if let Some(feat) = q.featured {
        filtered.retain(|p| p.featured == feat);
    }

    match q.sort.as_deref() {
        Some("price_asc")    => filtered.sort_by(|a, b| a.price.partial_cmp(&b.price).unwrap()),
        Some("price_desc")   => filtered.sort_by(|a, b| b.price.partial_cmp(&a.price).unwrap()),
        Some("rating")       => filtered.sort_by(|a, b| b.rating.partial_cmp(&a.rating).unwrap()),
        Some("reviews")      => filtered.sort_by(|a, b| b.review_count.cmp(&a.review_count)),
        _                    => {} // default: id order
    }

    let total = filtered.len() as u32;
    let total_pages = (total + per_page - 1) / per_page;
    let offset = ((page - 1) * per_page) as usize;

    let products: Vec<Product> = filtered
        .into_iter()
        .skip(offset)
        .take(per_page as usize)
        .cloned()
        .collect();

    Json(ProductPage { products, total, page, per_page, total_pages })
}

async fn get_product(State(db): State<Db>, Path(id): Path<u32>) -> Result<Json<Product>, axum::http::StatusCode> {
    db.iter()
        .find(|p| p.id == id)
        .cloned()
        .map(Json)
        .ok_or(axum::http::StatusCode::NOT_FOUND)
}

async fn list_categories(State(db): State<Db>) -> Json<CategoryList> {
    use std::collections::HashMap;
    let mut map: HashMap<String, std::collections::HashSet<String>> = HashMap::new();
    for p in db.iter() {
        map.entry(p.category.clone()).or_default().insert(p.subcategory.clone());
    }
    let mut categories: Vec<Category> = map.into_iter().map(|(name, subs)| {
        let count = db.iter().filter(|p| p.category == name).count() as u32;
        let mut subcategories: Vec<String> = subs.into_iter().collect();
        subcategories.sort();
        Category { name, subcategories, count }
    }).collect();
    categories.sort_by(|a, b| a.name.cmp(&b.name));
    Json(CategoryList { categories })
}

async fn featured_products(State(db): State<Db>) -> Json<Vec<Product>> {
    Json(db.iter().filter(|p| p.featured).take(12).cloned().collect())
}

async fn health() -> &'static str { "ok" }

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();
    let port: u16 = std::env::var("PORT").unwrap_or_else(|_| "8001".into()).parse().unwrap_or(8001);

    let db: Db = Arc::new(generate_products());
    tracing::info!("product-svc: loaded {} products", db.len());

    let cors = CorsLayer::new().allow_methods([Method::GET]).allow_origin(Any);

    let app = Router::new()
        .route("/health",           get(health))
        .route("/products",         get(list_products))
        .route("/products/{id}",    get(get_product))
        .route("/categories",       get(list_categories))
        .route("/featured",         get(featured_products))
        .layer(cors)
        .with_state(db);

    let addr = format!("0.0.0.0:{}", port);
    tracing::info!("product-svc listening on {}", addr);
    let listener = tokio::net::TcpListener::bind(&addr).await.unwrap();
    axum::serve(listener, app).await.unwrap();
}
