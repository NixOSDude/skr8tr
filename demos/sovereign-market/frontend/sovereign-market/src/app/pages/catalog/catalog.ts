import { Component, OnInit, inject } from '@angular/core';
import { ActivatedRoute, Router } from '@angular/router';
import { FormsModule } from '@angular/forms';
import { CommonModule } from '@angular/common';
import { ProductService, Product, ProductPage, Category } from '../../services/product';
import { ProductCardComponent } from '../../components/product-card/product-card';

@Component({
  selector: 'app-catalog',
  standalone: true,
  imports: [FormsModule, CommonModule, ProductCardComponent],
  templateUrl: './catalog.html',
  styleUrl: './catalog.scss',
})
export class CatalogComponent implements OnInit {
  private productSvc = inject(ProductService);
  private route = inject(ActivatedRoute);
  private router = inject(Router);

  result: ProductPage | null = null;
  categories: Category[] = [];
  loading = true;

  // filters
  selectedCategory = '';
  selectedSort = '';
  minPrice: number | null = null;
  maxPrice: number | null = null;
  currentPage = 1;
  perPage = 24;

  ngOnInit() {
    this.productSvc.categories().subscribe(c => this.categories = c.categories);
    this.route.queryParams.subscribe(p => {
      this.selectedCategory = p['category'] || '';
      this.currentPage = +p['page'] || 1;
      this.load();
    });
  }

  load() {
    this.loading = true;
    this.productSvc.list({
      page: this.currentPage,
      per_page: this.perPage,
      category: this.selectedCategory || undefined,
      sort: this.selectedSort || undefined,
      min_price: this.minPrice ?? undefined,
      max_price: this.maxPrice ?? undefined,
    }).subscribe(r => { this.result = r; this.loading = false; });
  }

  applyFilters() { this.currentPage = 1; this.load(); }
  clearFilters() { this.selectedCategory=''; this.selectedSort=''; this.minPrice=null; this.maxPrice=null; this.currentPage=1; this.load(); }
  goPage(p: number) { this.currentPage = p; this.load(); window.scrollTo(0,0); }
  pages(): number[] {
    if (!this.result) return [];
    const total = this.result.total_pages;
    const cur = this.currentPage;
    const range: number[] = [];
    for (let i = Math.max(1, cur-2); i <= Math.min(total, cur+2); i++) range.push(i);
    return range;
  }
}
