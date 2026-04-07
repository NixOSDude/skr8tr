import { Injectable, signal } from '@angular/core';
import { HttpClient } from '@angular/common/http';
import { Observable, tap } from 'rxjs';
import { environment } from '../../environments/environment';

export interface User { id: string; email: string; name: string; role: string; created_at: string; }
export interface AuthResponse { user: User; token: string; }

@Injectable({ providedIn: 'root' })
export class UserService {
  private base = environment.userSvcUrl;
  private _user = signal<User | null>(null);
  readonly user = this._user.asReadonly();

  constructor(private http: HttpClient) {
    const stored = localStorage.getItem('skr8tr_user');
    if (stored) this._user.set(JSON.parse(stored));
  }

  register(email: string, name: string, password: string): Observable<AuthResponse> {
    return this.http.post<AuthResponse>(`${this.base}/register`, { email, name, password }).pipe(
      tap(r => { this._user.set(r.user); localStorage.setItem('skr8tr_user', JSON.stringify(r.user)); })
    );
  }

  login(email: string, password: string): Observable<AuthResponse> {
    return this.http.post<AuthResponse>(`${this.base}/login`, { email, password }).pipe(
      tap(r => { this._user.set(r.user); localStorage.setItem('skr8tr_user', JSON.stringify(r.user)); })
    );
  }

  logout() { this._user.set(null); localStorage.removeItem('skr8tr_user'); }
  isLoggedIn() { return this._user() !== null; }
}
