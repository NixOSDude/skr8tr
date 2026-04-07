import { Component, OnInit, inject } from '@angular/core';
import { ActivatedRoute, RouterLink } from '@angular/router';
import { CommonModule } from '@angular/common';
import { ProductService, Product } from '../../services/product';
import { ReviewService, ReviewSummary } from '../../services/review';
import { RecommendationService, RecommendedProduct } from '../../services/recommendation';
import { CartService } from '../../services/cart';

@Component({
  selector: 'app-product-detail',
  standalone: true,
  imports: [CommonModule, RouterLink],
  templateUrl: './product-detail.html',
  styleUrl: './product-detail.scss',
})
export class ProductDetailComponent implements OnInit {
  private route = inject(ActivatedRoute);
  private productSvc = inject(ProductService);
  private reviewSvc = inject(ReviewService);
  private recoSvc = inject(RecommendationService);
  private cartSvc = inject(CartService);

  product: Product | null = null;
  reviews: ReviewSummary | null = null;
  related: RecommendedProduct[] = [];
  quantity = 1;
  adding = false;
  added = false;

  ngOnInit() {
    this.route.params.subscribe(p => {
      const id = +p['id'];
      this.productSvc.get(id).subscribe(p => { this.product = p; });
      this.reviewSvc.get(id).subscribe(r => this.reviews = r);
      this.recoSvc.related(id).subscribe(r => this.related = r);
    });
  }

  stars(rating: number): string { return '★'.repeat(Math.round(rating)) + '☆'.repeat(5 - Math.round(rating)); }
  ratingPct(dist: Record<number, number>, star: number): number {
    if (!this.reviews) return 0;
    return Math.round(((dist[star] ?? 0) / Math.max(this.reviews.total_reviews, 1)) * 100);
  }

  addToCart() {
    if (!this.product || !this.product.in_stock) return;
    this.adding = true;
    this.cartSvc.addItem(this.product.id, this.product.name, this.product.price, this.product.image_url, this.quantity)
      .subscribe(() => { this.adding = false; this.added = true; setTimeout(() => this.added = false, 2500); });
  }
}
