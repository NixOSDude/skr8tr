import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../environments/environment';

export interface Review {
  id: string; product_id: number; user_id: string; user_name: string;
  rating: number; title: string; body: string; verified: boolean; created_at: string;
}

export interface ReviewSummary {
  product_id: number; average_rating: number; total_reviews: number;
  distribution: Record<number, number>; reviews: Review[];
}

@Injectable({ providedIn: 'root' })
export class ReviewService {
  private base = environment.reviewSvcUrl;
  constructor(private http: HttpClient) {}
  get(productId: number): Observable<ReviewSummary> {
    return this.http.get<ReviewSummary>(`${this.base}/reviews/${productId}`);
  }
  submit(productId: number, review: Partial<Review>): Observable<Review> {
    return this.http.post<Review>(`${this.base}/reviews/${productId}`, review);
  }
}
