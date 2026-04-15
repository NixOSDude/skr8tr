import { Injectable } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../environments/environment';

export interface Product {
  id: number; sku: string; name: string; description: string;
  price: number; category: string; subcategory: string; brand: string;
  rating: number; review_count: number; image_url: string;
  tags: string[]; featured: boolean; in_stock: boolean;
}

export interface ProductPage {
  products: Product[]; total: number; page: number;
  per_page: number; total_pages: number;
}

export interface Category {
  name: string; subcategories: string[]; count: number;
}

export interface ProductQuery {
  page?: number; per_page?: number; category?: string;
  min_price?: number; max_price?: number; brand?: string;
  featured?: boolean; sort?: string;
}

@Injectable({ providedIn: 'root' })
export class ProductService {
  private base = environment.productSvcUrl;
  constructor(private http: HttpClient) {}

  list(q: ProductQuery = {}): Observable<ProductPage> {
    let params = new HttpParams();
    if (q.page)      params = params.set('page', q.page);
    if (q.per_page)  params = params.set('per_page', q.per_page);
    if (q.category)  params = params.set('category', q.category);
    if (q.min_price !== undefined) params = params.set('min_price', q.min_price);
    if (q.max_price !== undefined) params = params.set('max_price', q.max_price);
    if (q.brand)     params = params.set('brand', q.brand);
    if (q.featured !== undefined) params = params.set('featured', q.featured);
    if (q.sort)      params = params.set('sort', q.sort);
    return this.http.get<ProductPage>(`${this.base}/products`, { params });
  }

  get(id: number): Observable<Product> {
    return this.http.get<Product>(`${this.base}/products/${id}`);
  }

  categories(): Observable<{ categories: Category[] }> {
    return this.http.get<{ categories: Category[] }>(`${this.base}/categories`);
  }

  featured(): Observable<Product[]> {
    return this.http.get<Product[]>(`${this.base}/featured`);
  }
}
