import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../environments/environment';

export interface RecommendedProduct {
  id: number; name: string; price: number;
  rating: number; image_url: string; reason: string;
}

@Injectable({ providedIn: 'root' })
export class RecommendationService {
  private base = environment.recommendationSvcUrl;
  constructor(private http: HttpClient) {}
  related(productId: number, limit = 8): Observable<RecommendedProduct[]> {
    return this.http.get<RecommendedProduct[]>(`${this.base}/recommendations/${productId}?limit=${limit}`);
  }
  trending(): Observable<RecommendedProduct[]> {
    return this.http.get<RecommendedProduct[]>(`${this.base}/trending`);
  }
}
