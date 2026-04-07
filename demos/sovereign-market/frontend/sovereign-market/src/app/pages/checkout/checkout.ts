import { Component, inject } from '@angular/core';
import { RouterLink, Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CommonModule } from '@angular/common';
import { CartService } from '../../services/cart';
import { OrderService } from '../../services/order';

@Component({
  selector: 'app-checkout',
  standalone: true,
  imports: [RouterLink, FormsModule, CommonModule],
  templateUrl: './checkout.html',
  styleUrl: './checkout.scss',
})
export class CheckoutComponent {
  private cartSvc = inject(CartService);
  private orderSvc = inject(OrderService);
  private router = inject(Router);

  cart = this.cartSvc.cart;
  placing = false;
  error = '';

  form = { name:'', street:'', city:'', state:'', zip:'', country:'US', email:'' };

  placeOrder() {
    const c = this.cart();
    if (!c || c.items.length === 0) return;
    this.placing = true;
    const items = c.items.map(i => ({ product_id: i.product_id, name: i.name, price: i.price, quantity: i.quantity }));
    const addr = { name: this.form.name, street: this.form.street, city: this.form.city, state: this.form.state, zip: this.form.zip, country: this.form.country };
    this.orderSvc.place('guest-user', items, addr).subscribe({
      next: (order) => {
        this.cartSvc.clear().subscribe();
        this.router.navigate(['/orders'], { queryParams: { new: order.id } });
      },
      error: () => { this.error = 'Order failed. Try again.'; this.placing = false; }
    });
  }
}
