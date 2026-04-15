import { Component, OnInit, inject } from '@angular/core';
import { ActivatedRoute, RouterLink } from '@angular/router';
import { CommonModule } from '@angular/common';
import { SearchService, SearchResult } from '../../services/search';

@Component({ selector:'app-search-results', standalone:true, imports:[RouterLink,CommonModule], templateUrl:'./search-results.html', styleUrl:'./search-results.scss' })
export class SearchResultsComponent implements OnInit {
  private searchSvc = inject(SearchService);
  private route = inject(ActivatedRoute);
  result: SearchResult | null = null;
  query = '';
  loading = true;
  ngOnInit() {
    this.route.queryParams.subscribe(p => {
      this.query = p['q'] || '';
      if (this.query) { this.loading = true; this.searchSvc.search(this.query).subscribe(r => { this.result = r; this.loading = false; }); }
    });
  }
  stars(r: number) { return '★'.repeat(Math.round(r)) + '☆'.repeat(5-Math.round(r)); }
}
