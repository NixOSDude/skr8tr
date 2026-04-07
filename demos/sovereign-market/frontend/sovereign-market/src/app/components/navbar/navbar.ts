import { Component, inject } from '@angular/core';
import { RouterLink } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { CartService } from '../../services/cart';
import { UserService } from '../../services/user';

@Component({
  selector: 'app-navbar',
  standalone: true,
  imports: [RouterLink, FormsModule],
  templateUrl: './navbar.html',
  styleUrl: './navbar.scss',
})
export class NavbarComponent {
  private router = inject(Router);
  private cartSvc = inject(CartService);
  private userSvc = inject(UserService);

  searchQuery = '';
  showLogin = false;

  cart = this.cartSvc.cart;
  user = this.userSvc.user;

  doSearch() {
    if (this.searchQuery.trim()) {
      this.router.navigate(['/search'], { queryParams: { q: this.searchQuery.trim() } });
    }
  }

  logout() { this.userSvc.logout(); }
}
