import { Component, inject } from '@angular/core';
import { RouterLink } from '@angular/router';
import { CommonModule } from '@angular/common';
import { CartService } from '../../services/cart';

@Component({
  selector: 'app-cart',
  standalone: true,
  imports: [RouterLink, CommonModule],
  templateUrl: './cart.html',
  styleUrl: './cart.scss',
})
export class CartComponent {
  private cartSvc = inject(CartService);
  cart = this.cartSvc.cart;

  remove(productId: number) { this.cartSvc.removeItem(productId).subscribe(); }
  clear() { this.cartSvc.clear().subscribe(); }
}
