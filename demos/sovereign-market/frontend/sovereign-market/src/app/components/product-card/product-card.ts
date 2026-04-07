import { Component, Input, inject } from '@angular/core';
import { RouterLink } from '@angular/router';
import { Product } from '../../services/product';
import { CartService } from '../../services/cart';

@Component({
  selector: 'app-product-card',
  standalone: true,
  imports: [RouterLink],
  templateUrl: './product-card.html',
  styleUrl: './product-card.scss',
})
export class ProductCardComponent {
  @Input() product!: Product;
  private cartSvc = inject(CartService);
  adding = false;

  addToCart(e: Event) {
    e.preventDefault();
    e.stopPropagation();
    if (!this.product.in_stock) return;
    this.adding = true;
    this.cartSvc.addItem(this.product.id, this.product.name, this.product.price, this.product.image_url)
      .subscribe(() => { this.adding = false; });
  }

  stars(rating: number): string { return '★'.repeat(Math.round(rating)) + '☆'.repeat(5 - Math.round(rating)); }
}
