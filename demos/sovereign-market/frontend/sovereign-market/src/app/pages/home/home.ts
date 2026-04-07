import { Component, OnInit, inject } from '@angular/core';
import { RouterLink } from '@angular/router';
import { CommonModule } from '@angular/common';
import { ProductService, Product, Category } from '../../services/product';
import { RecommendationService, RecommendedProduct } from '../../services/recommendation';
import { CategoryCountPipe } from '../../pipes/category-count.pipe';
import { ProductCardComponent } from '../../components/product-card/product-card';

@Component({
  selector: 'app-home',
  standalone: true,
  imports: [RouterLink, CommonModule, ProductCardComponent, CategoryCountPipe],
  templateUrl: './home.html',
  styleUrl: './home.scss',
})
export class HomeComponent implements OnInit {
  private productSvc = inject(ProductService);
  private recoSvc = inject(RecommendationService);

  featured: Product[] = [];
  categories: Category[] = [];
  trending: RecommendedProduct[] = [];
  loading = true;

  ngOnInit() {
    this.productSvc.featured().subscribe(f => { this.featured = f; this.loading = false; });
    this.productSvc.categories().subscribe(c => this.categories = c.categories.slice(0, 7));
    this.recoSvc.trending().subscribe(t => this.trending = t);
  }

  heroCategories = [
    { name: 'Electronics', icon: '💻', color: '#3b82f6' },
    { name: 'Fashion', icon: '👗', color: '#ec4899' },
    { name: 'Home & Garden', icon: '🏡', color: '#10b981' },
    { name: 'Sports & Outdoors', icon: '⚽', color: '#f59e0b' },
    { name: 'Books & Media', icon: '📚', color: '#8b5cf6' },
    { name: 'Health & Beauty', icon: '💊', color: '#ef4444' },
    { name: 'Automotive', icon: '🚗', color: '#64748b' },
  ];
}
