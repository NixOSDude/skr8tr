import { Injectable } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable } from 'rxjs';
import { environment } from '../../environments/environment';

export interface OrderItem { product_id: number; name: string; price: number; quantity: number; }
export interface ShippingAddress { name: string; street: string; city: string; state: string; zip: string; country: string; }
export interface Order {
  id: string; user_id: string; items: OrderItem[]; subtotal: number;
  shipping: number; tax: number; total: number; status: string;
  shipping_address: ShippingAddress; created_at: string;
}

@Injectable({ providedIn: 'root' })
export class OrderService {
  private base = environment.orderSvcUrl;
  constructor(private http: HttpClient) {}
  place(userId: string, items: OrderItem[], address: ShippingAddress): Observable<Order> {
    return this.http.post<Order>(`${this.base}/orders`, { user_id: userId, items, shipping_address: address });
  }
  get(id: string): Observable<Order> {
    return this.http.get<Order>(`${this.base}/orders/${id}`);
  }
  userOrders(userId: string): Observable<Order[]> {
    return this.http.get<Order[]>(`${this.base}/orders/user/${userId}`);
  }
}
