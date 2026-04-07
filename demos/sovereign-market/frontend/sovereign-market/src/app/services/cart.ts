import { Injectable, signal } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable, tap } from 'rxjs';
import { environment } from '../../environments/environment';

export interface CartItem {
  product_id: number; name: string; price: number;
  quantity: number; image_url: string;
}

export interface Cart {
  session_id: string; items: CartItem[];
  subtotal: number; item_count: number;
}

@Injectable({ providedIn: 'root' })
export class CartService {
  private base = environment.cartSvcUrl;
  private _sessionId = signal<string>('');
  private _cart = signal<Cart | null>(null);

  readonly sessionId = this._sessionId.asReadonly();
  readonly cart = this._cart.asReadonly();

  constructor(private http: HttpClient) {
    const stored = localStorage.getItem('skr8tr_session');
    if (stored) {
      this._sessionId.set(stored);
      this.load().subscribe(c => this._cart.set(c));
    } else {
      this.newSession();
    }
  }

  private newSession() {
    this.http.post<{session_id: string}>(`${this.base}/session`, {}).subscribe(r => {
      this._sessionId.set(r.session_id);
      localStorage.setItem('skr8tr_session', r.session_id);
    });
  }

  load(): Observable<Cart> {
    return this.http.get<Cart>(`${this.base}/cart/${this._sessionId()}`).pipe(
      tap(c => this._cart.set(c))
    );
  }

  addItem(productId: number, name: string, price: number, imageUrl: string, qty = 1): Observable<Cart> {
    return this.http.post<Cart>(`${this.base}/cart/${this._sessionId()}/items`, {
      product_id: productId, name, price, quantity: qty, image_url: imageUrl
    }).pipe(tap(c => this._cart.set(c)));
  }

  removeItem(productId: number): Observable<Cart> {
    return this.http.delete<Cart>(`${this.base}/cart/${this._sessionId()}/items/${productId}`)
      .pipe(tap(c => this._cart.set(c)));
  }

  clear(): Observable<Cart> {
    return this.http.delete<Cart>(`${this.base}/cart/${this._sessionId()}`)
      .pipe(tap(c => this._cart.set(c)));
  }
}
