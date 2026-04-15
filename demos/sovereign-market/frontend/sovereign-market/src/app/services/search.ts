import { Injectable } from '@angular/core';
import { HttpClient, HttpParams } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../environments/environment';

export interface SearchProduct {
  id: number; name: string; category: string;
  brand: string; price: number; rating: number; image_url: string;
}

export interface SearchResult {
  query: string; results: SearchProduct[];
  total: number; page: number; suggestions: string[];
}

@Injectable({ providedIn: 'root' })
export class SearchService {
  private base = environment.searchSvcUrl;
  constructor(private http: HttpClient) {}

  search(q: string, page = 1, perPage = 20): Observable<SearchResult> {
    const params = new HttpParams().set('q', q).set('page', page).set('per_page', perPage);
    return this.http.get<SearchResult>(`${this.base}/search`, { params });
  }
}
