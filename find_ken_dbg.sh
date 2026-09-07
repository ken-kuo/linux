#!/bin/bash

# 获取所有本地分支名称  
branches=$(git branch | sed 's/^..//')

# 遍历每个分支  
for branch in $branches; do  
  echo "Checking branch: $branch"

  # 获取最近 30 个提交的详细补丁信息并查找 "KEN_DBG"
  if git log -n 30 -p $branch | grep -iq "KEN_DBG"; then  
    echo "Match found in branch $branch"
    break  
  fi  
done

