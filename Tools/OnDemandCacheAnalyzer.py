#!/usr/bin/env python3
"""
OnDemand Cache Analyzer
解析 UE IoStoreOnDemand 下载缓存目录

文件格式参考:
- cas.jrn: CAS Journal, 包含 chunk location 和 block 操作记录
- ias.cache.0: IAS 缓存数据块
- ias.cache.0.jrn: IAS 缓存日志
"""

import os
import sys
import struct
from pathlib import Path
from datetime import datetime, timezone
from typing import List, Dict, Any, Optional
from dataclasses import dataclass
import hashlib

@dataclass
class CasJournalHeader:
    magic: bytes
    version: int
    
@dataclass
class CasJournalEntry:
    entry_type: int
    data: bytes
    
@dataclass
class ChunkLocation:
    cas_location: bytes
    cas_addr: bytes
    
@dataclass
class BlockOperation:
    block_id: int
    utc_ticks: int

CAS_JOURNAL_HEADER_MAGIC = b'CASJOURNALHEADER'
CAS_JOURNAL_FOOTER_MAGIC = b'CASJOURNALFOOTER'

ENTRY_TYPES = {
    0: 'None',
    1: 'ChunkLocation',
    2: 'BlockCreated',
    3: 'BlockDeleted',
    4: 'BlockAccess',
    5: 'CriticalError'
}

def parse_cas_journal(filepath: str) -> Dict[str, Any]:
    """解析 CAS Journal 文件"""
    result = {
        'file': filepath,
        'size': os.path.getsize(filepath),
        'header': None,
        'entries': [],
        'footer_valid': False
    }
    
    with open(filepath, 'rb') as f:
        # 读取 Header (32 bytes)
        header_data = f.read(32)
        if len(header_data) >= 16:
            magic = header_data[:16]
            if magic == CAS_JOURNAL_HEADER_MAGIC:
                version = struct.unpack('<I', header_data[16:20])[0] if len(header_data) >= 20 else 0
                result['header'] = {
                    'magic': 'CASJOURNALHEADER',
                    'version': version
                }
            else:
                result['header'] = {
                    'magic': magic.hex(),
                    'version': 'Unknown format'
                }
                return result
        
        # 读取 Entries (每个 24 bytes)
        while True:
            entry_data = f.read(24)
            if len(entry_data) < 24:
                break
            
            entry_type = entry_data[0]
            
            if entry_type == 1:  # ChunkLocation
                cas_location = entry_data[4:12]
                cas_addr = entry_data[12:24]
                result['entries'].append({
                    'type': 'ChunkLocation',
                    'cas_location': cas_location.hex(),
                    'cas_addr': cas_addr.hex()
                })
            elif entry_type == 2:  # BlockCreated
                block_id = struct.unpack('<Q', entry_data[4:12])[0]
                result['entries'].append({
                    'type': 'BlockCreated',
                    'block_id': block_id
                })
            elif entry_type == 3:  # BlockDeleted
                block_id = struct.unpack('<Q', entry_data[4:12])[0]
                result['entries'].append({
                    'type': 'BlockDeleted', 
                    'block_id': block_id
                })
            elif entry_type == 4:  # BlockAccess
                block_id = struct.unpack('<Q', entry_data[4:12])[0]
                utc_ticks = struct.unpack('<q', entry_data[12:20])[0]
                dt = datetime.fromtimestamp(utc_ticks / 10_000_000 - 11644473600, tz=timezone.utc) if utc_ticks > 0 else None
                result['entries'].append({
                    'type': 'BlockAccess',
                    'block_id': block_id,
                    'utc_ticks': utc_ticks,
                    'datetime': str(dt) if dt else 'N/A'
                })
            elif entry_type == 5:  # CriticalError
                error_code = struct.unpack('<I', entry_data[1:5])[0]
                result['entries'].append({
                    'type': 'CriticalError',
                    'error_code': error_code
                })
        
        # 检查 Footer
        f.seek(-16, 2)
        footer = f.read(16)
        result['footer_valid'] = footer == CAS_JOURNAL_FOOTER_MAGIC
    
    return result

def parse_ias_cache_journal(filepath: str) -> Dict[str, Any]:
    """解析 IAS Cache Journal 文件"""
    result = {
        'file': filepath,
        'size': os.path.getsize(filepath),
        'entries': [],
        'total_entries': 0
    }
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    # 每个 entry 约 90 bytes (基于观察)
    entry_size = 90  # 估计值
    num_entries = len(data) // entry_size
    result['total_entries'] = num_entries
    
    # 简单统计
    result['raw_size'] = len(data)
    
    return result

def analyze_directory(dirpath: str) -> Dict[str, Any]:
    """分析整个下载缓存目录"""
    result = {
        'directory': dirpath,
        'files': [],
        'summary': {
            'total_size': 0,
            'total_files': 0,
            'cas_entries': 0,
            'chunk_locations': 0,
            'blocks_created': 0,
            'blocks_deleted': 0,
            'blocks_accessed': 0
        }
    }
    
    for root, dirs, files in os.walk(dirpath):
        for file in files:
            filepath = os.path.join(root, file)
            relpath = os.path.relpath(filepath, dirpath)
            size = os.path.getsize(filepath)
            
            result['summary']['total_size'] += size
            result['summary']['total_files'] += 1
            
            file_info = {
                'path': relpath,
                'size': size,
                'size_mb': round(size / 1024 / 1024, 2)
            }
            
            if file == 'cas.jrn':
                parsed = parse_cas_journal(filepath)
                file_info['type'] = 'CAS Journal'
                file_info['header'] = parsed['header']
                file_info['footer_valid'] = parsed['footer_valid']
                file_info['entry_count'] = len(parsed['entries'])
                
                for entry in parsed['entries']:
                    result['summary']['cas_entries'] += 1
                    if entry['type'] == 'ChunkLocation':
                        result['summary']['chunk_locations'] += 1
                    elif entry['type'] == 'BlockCreated':
                        result['summary']['blocks_created'] += 1
                    elif entry['type'] == 'BlockDeleted':
                        result['summary']['blocks_deleted'] += 1
                    elif entry['type'] == 'BlockAccess':
                        result['summary']['blocks_accessed'] += 1
                
            elif file.endswith('.jrn') and 'ias.cache' in file:
                parsed = parse_ias_cache_journal(filepath)
                file_info['type'] = 'IAS Cache Journal'
                file_info['entries'] = parsed['total_entries']
                
            elif 'ias.cache' in file and not file.endswith('.jrn'):
                file_info['type'] = 'IAS Cache Data'
                
            elif 'blocks' in root:
                file_info['type'] = 'Cache Block'
                
            result['files'].append(file_info)
    
    return result

def format_size(size: int) -> str:
    """格式化文件大小"""
    if size < 1024:
        return f'{size} B'
    elif size < 1024 * 1024:
        return f'{size / 1024:.2f} KB'
    elif size < 1024 * 1024 * 1024:
        return f'{size / 1024 / 1024:.2f} MB'
    else:
        return f'{size / 1024 / 1024 / 1024:.2f} GB'

def print_report(result: Dict[str, Any], verbose: bool = False):
    """打印分析报告"""
    print('=' * 60)
    print('IoStoreOnDemand Cache Analysis Report')
    print('=' * 60)
    print(f"\nDirectory: {result['directory']}")
    print(f"\n{'Summary':-^40}")
    print(f"  Total Files: {result['summary']['total_files']}")
    print(f"  Total Size:  {format_size(result['summary']['total_size'])}")
    print(f"\n{'CAS Journal Statistics':-^40}")
    print(f"  Total Entries:      {result['summary']['cas_entries']}")
    print(f"  Chunk Locations:   {result['summary']['chunk_locations']}")
    print(f"  Blocks Created:    {result['summary']['blocks_created']}")
    print(f"  Blocks Deleted:    {result['summary']['blocks_deleted']}")
    print(f"  Blocks Accessed:   {result['summary']['blocks_accessed']}")
    
    print(f"\n{'Files':-^40}")
    for f in result['files']:
        print(f"  [{f.get('type', 'Unknown'):20}] {f['path']}")
        print(f"    Size: {format_size(f['size'])}")
        if 'header' in f and f['header']:
            print(f"    Header: {f['header']}")
        if 'entry_count' in f:
            print(f"    Entries: {f['entry_count']}")
        if 'footer_valid' in f:
            print(f"    Footer Valid: {f['footer_valid']}")
    
    print('=' * 60)

def main():
    if len(sys.argv) < 2:
        print("Usage: python OnDemandCacheAnalyzer.py <cache_directory> [-v]")
        print("\nExample:")
        print('  python OnDemandCacheAnalyzer.py "E:\\UEProject\\IOStoreDemo\\Archives\\Windows\\IOStoreDemo\\Saved\\PersistentDownloadDir"')
        sys.exit(1)
    
    dirpath = sys.argv[1]
    verbose = '-v' in sys.argv or '--verbose' in sys.argv
    
    if not os.path.isdir(dirpath):
        print(f"Error: Directory not found: {dirpath}")
        sys.exit(1)
    
    result = analyze_directory(dirpath)
    print_report(result, verbose)
    
    if verbose:
        print(f"\n{'Detailed Entries':-^40}")
        for f in result['files']:
            if f.get('type') == 'CAS Journal':
                # 重新解析获取详细 entries
                parsed = parse_cas_journal(os.path.join(dirpath, f['path']))
                for i, entry in enumerate(parsed['entries'][:20]):  # 只显示前20条
                    print(f"  [{i:3}] {entry['type']}")
                    if entry['type'] == 'ChunkLocation':
                        print(f"        Location: {entry['cas_location'][:16]}...")
                    elif entry['type'] == 'BlockAccess':
                        print(f"        Block: {entry['block_id']}, Time: {entry.get('datetime', 'N/A')}")
                if len(parsed['entries']) > 20:
                    print(f"  ... and {len(parsed['entries']) - 20} more entries")

if __name__ == '__main__':
    main()
