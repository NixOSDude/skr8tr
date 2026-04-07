import { Component, OnInit, inject } from '@angular/core';
import { ActivatedRoute, RouterLink } from '@angular/router';
import { CommonModule } from '@angular/common';
import { OrderService, Order } from '../../services/order';

@Component({ selector:'app-orders', standalone:true, imports:[RouterLink,CommonModule], templateUrl:'./orders.html', styleUrl:'./orders.scss' })
export class OrdersComponent implements OnInit {
  private orderSvc = inject(OrderService);
  private route = inject(ActivatedRoute);
  orders: Order[] = [];
  newOrderId = '';
  ngOnInit() {
    this.route.queryParams.subscribe(p => { this.newOrderId = p['new'] || ''; });
    this.orderSvc.userOrders('guest-user').subscribe(o => this.orders = o);
  }
}
