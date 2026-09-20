#!/usr/bin/env python3
"""
将生成的JPG植物图片转换为PNG并去除背景
要求：pip install pillow rembg
"""
import os
import sys
from pathlib import Path
from PIL import Image
from rembg import remove

def process_image(input_path, output_path):
    """处理单张图片：去背景 + 转PNG + 调整尺寸"""
    print(f"处理: {input_path.name} -> {output_path.name}")
    
    # 读取原图
    img = Image.open(input_path)
    
    # 去除背景
    img_no_bg = remove(img)
    
    # 确保是RGBA模式
    if img_no_bg.mode != 'RGBA':
        img_no_bg = img_no_bg.convert('RGBA')
    
    # 调整尺寸为160x160（游戏使用SS=2，实际显示80x80）
    img_resized = img_no_bg.resize((160, 160), Image.Resampling.LANCZOS)
    
    # 保存为PNG
    img_resized.save(output_path, 'PNG', optimize=True)
    print(f"  ✓ 尺寸: {img_resized.size}, 模式: {img_resized.mode}")

def main():
    if len(sys.argv) < 2:
        print("用法: python convert_to_png.py <输入目录> [输出目录]")
        print("示例: python convert_to_png.py test_plants test_plants_png")
        sys.exit(1)
    
    input_dir = Path(sys.argv[1])
    output_dir = Path(sys.argv[2] if len(sys.argv) > 2 else f"{input_dir}_png")
    
    if not input_dir.exists():
        print(f"错误: 输入目录不存在: {input_dir}")
        sys.exit(1)
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # 查找所有JPG/JPEG文件
    image_files = list(input_dir.glob("*.jpg")) + list(input_dir.glob("*.jpeg"))
    
    if not image_files:
        print(f"错误: {input_dir} 中没有找到JPG图片")
        sys.exit(1)
    
    print(f"找到 {len(image_files)} 张图片")
    print("=" * 60)
    
    success = 0
    failed = []
    
    for img_file in sorted(image_files):
        try:
            output_file = output_dir / f"{img_file.stem}.png"
            process_image(img_file, output_file)
            success += 1
        except Exception as e:
            print(f"  ✗ 失败: {e}")
            failed.append(img_file.name)
    
    print("=" * 60)
    print(f"完成: {success}/{len(image_files)} 张成功")
    
    if failed:
        print(f"失败: {', '.join(failed)}")
    
    return 0 if not failed else 1

if __name__ == "__main__":
    sys.exit(main())
