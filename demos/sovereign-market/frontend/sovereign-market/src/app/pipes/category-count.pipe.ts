import { Pipe, PipeTransform } from '@angular/core';
import { Category } from '../services/product';

@Pipe({ name: 'categoryCount', standalone: true })
export class CategoryCountPipe implements PipeTransform {
  transform(categories: Category[], name: string): number {
    return categories.find(c => c.name === name)?.count ?? 0;
  }
}
