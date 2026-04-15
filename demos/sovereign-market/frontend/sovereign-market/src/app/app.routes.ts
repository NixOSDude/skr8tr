import { Routes } from '@angular/router';

export const routes: Routes = [
  { path: '', loadComponent: () => import('./pages/home/home').then(m => m.HomeComponent) },
  { path: 'catalog', loadComponent: () => import('./pages/catalog/catalog').then(m => m.CatalogComponent) },
  { path: 'product/:id', loadComponent: () => import('./pages/product-detail/product-detail').then(m => m.ProductDetailComponent) },
  { path: 'cart', loadComponent: () => import('./pages/cart/cart').then(m => m.CartComponent) },
  { path: 'checkout', loadComponent: () => import('./pages/checkout/checkout').then(m => m.CheckoutComponent) },
  { path: 'orders', loadComponent: () => import('./pages/orders/orders').then(m => m.OrdersComponent) },
  { path: 'search', loadComponent: () => import('./pages/search-results/search-results').then(m => m.SearchResultsComponent) },
  { path: '**', redirectTo: '' },
];
