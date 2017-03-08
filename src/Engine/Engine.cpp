/*
 * This file is part of Compare plugin for Notepad++
 * Copyright (C)2011 Jean-Sebastien Leroy (jean.sebastien.leroy@gmail.com)
 * Copyright (C)2017 Pavel Nedev (pg.nedev@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <climits>
#include <exception>

#include <windows.h>

#include "Engine.h"
#include "diff.h"
#include "ProgressDlg.h"


// Rotate a value n bits to the left
#define UINT_BIT (sizeof(unsigned) * CHAR_BIT)
#define ROL(v, n) ((v) << (n) | (v) >> (UINT_BIT - (n)))

// Given a hash value and a new character, return a new hash value
#define HASH(h, c) ((c) + ROL(h, 7))


namespace {

enum class charType
{
	SPACECHAR,
	ALPHANUMCHAR,
	OTHERCHAR
};


struct DocCmpInfo
{
	HWND		view;
	section_t	section;

	int			blockDiffMask;
};


struct CompareInfo
{
	// Input data
	DocCmpInfo				doc1;
	DocCmpInfo				doc2;

	// Output data - filled by the compare engine
	std::vector<diff_info>	diffBlocks;
};


struct Word
{
	charType type;

	int line;
	int pos;
	int length;

	unsigned int hash;

	inline bool operator==(const Word& rhs) const
	{
		return (hash == rhs.hash);
	}

	inline bool operator!=(const Word& rhs) const
	{
		return (hash != rhs.hash);
	}

	inline bool operator==(unsigned int rhs) const
	{
		return (hash == rhs);
	}

	inline bool operator!=(unsigned int rhs) const
	{
		return (hash != rhs);
	}
};


struct chunk_info
{
	chunk_info(int line_offset, int line_count) :
		lineStart(line_offset), lineCount(line_count),
		lineStartWordIdx(line_count), lineEndWordIdx(line_count), lineMappings(line_count, -1)
	{}

	int lineStart;
	int lineCount;

	std::vector<int> lineStartWordIdx;
	std::vector<int> lineEndWordIdx;
	std::vector<int> lineMappings;

	std::vector<Word> words;
};


std::vector<unsigned int> computeLineHashes(DocCmpInfo& doc, const UserSettings& settings)
{
	const int monitorCancelEachXLine = 500;

	progress_ptr& progress = ProgressDlg::Get();

	int lineCount = ::SendMessage(doc.view, SCI_GETLENGTH, 0, 0);

	if (lineCount)
		lineCount = ::SendMessage(doc.view, SCI_GETLINECOUNT, 0, 0);

	if ((doc.section.len <= 0) || (doc.section.off + doc.section.len > lineCount))
		doc.section.len = lineCount - doc.section.off;

	std::vector<unsigned int> lineHashes(doc.section.len);

	for (int lineNum = 0; lineNum < doc.section.len; ++lineNum)
	{
		if (progress && (lineNum % monitorCancelEachXLine == 0) && progress->IsCancelled())
			return std::vector<unsigned int>{};

		const int lineStart = ::SendMessage(doc.view, SCI_POSITIONFROMLINE, lineNum + doc.section.off, 0);
		const int lineEnd = ::SendMessage(doc.view, SCI_GETLINEENDPOSITION, lineNum + doc.section.off, 0);

		unsigned int hash = 0;

		if (lineEnd - lineStart)
		{
			std::vector<char> line = getText(doc.view, lineStart, lineEnd);
			const int lineLen = static_cast<int>(line.size()) - 1;

			if (settings.IgnoreCase)
				toLowerCase(line);

			for (int i = 0; i < lineLen; ++i)
			{
				if (settings.IgnoreSpaces && (line[i] == ' ' || line[i] == '\t'))
					continue;

				hash = HASH(hash, line[i]);
			}
		}

		lineHashes[lineNum] = hash;
	}

	return lineHashes;
}


charType getCharType(char letter)
{
	if (letter == ' ' || letter == '\t')
		return charType::SPACECHAR;

	if (::IsCharAlphaNumericA(letter) || letter == '_')
		return charType::ALPHANUMCHAR;

	return charType::OTHERCHAR;
}


void getWords(HWND view, const UserSettings& settings, chunk_info& chunk)
{
	chunk.words.clear();

	for (int lineNum = 0; lineNum < chunk.lineCount; ++lineNum)
	{
		chunk.lineStartWordIdx[lineNum] = static_cast<int>(chunk.words.size());

		const int docLineNum = lineNum + chunk.lineStart;
		const int docLineStart = ::SendMessage(view, SCI_POSITIONFROMLINE, docLineNum, 0);
		const int docLineEnd = ::SendMessage(view, SCI_GETLINEENDPOSITION, docLineNum, 0);

		std::vector<char> line = getText(view, docLineStart, docLineEnd);
		const int lineLen = static_cast<int>(line.size()) - 1;

		if (lineLen > 0)
		{
			if (settings.IgnoreCase)
				toLowerCase(line);

			Word word;
			word.type = getCharType(line[0]);
			word.hash = HASH(0, line[0]);
			word.line = lineNum;
			word.pos = 0;
			word.length = 1;

			for (int i = 1; i < lineLen; ++i)
			{
				const char l = line[i];
				charType newType = getCharType(l);

				if (newType == word.type)
				{
					++word.length;
					word.hash = HASH(word.hash, l);
				}
				else
				{
					if (!settings.IgnoreSpaces || word.type != charType::SPACECHAR)
						chunk.words.push_back(word);

					word.type = newType;
					word.hash = HASH(0, l);
					word.pos = i;
					word.length = 1;
				}
			}

			if (!settings.IgnoreSpaces || word.type != charType::SPACECHAR)
				chunk.words.push_back(word);
		}

		chunk.lineEndWordIdx[lineNum] = static_cast<int>(chunk.words.size());
	}
}


int getBestMatchingSubChunkOffset(const chunk_info* shorterChunk, const chunk_info* longerChunk)
{
	const int endLine = longerChunk->lineCount - shorterChunk->lineCount;

	if (endLine <= 0)
		return 0;

	int bestMatchingSubChunkOffset = -1;

	for (int step = endLine + 1, line = 0, bestMatchWordsCount = 0;
			line <= endLine && line != bestMatchingSubChunkOffset;)
	{
		// Sub-chunk to test
		const std::vector<Word>
				words2(longerChunk->words.begin() + longerChunk->lineStartWordIdx[line], longerChunk->words.end());

		const std::vector<Word>* pWords1 = &(shorterChunk->words);
		const std::vector<Word>* pWords2 = &words2;

		if (pWords1->size() > pWords2->size())
			std::swap(pWords1, pWords2);

		// Compare the two chunks
		const std::vector<diff_info> chunkDiff = DiffCalc<Word>(*pWords1, *pWords2)();

		const int chunkDiffSize = static_cast<int>(chunkDiff.size());

		int matchingWordsCount = 0;

		// Count the number of matching words
		for (int i = 0; i < chunkDiffSize; ++i)
		{
			const diff_info& cd = chunkDiff[i];

			if (cd.type == diff_type::DIFF_MATCH)
			{
				for (int wordIdx = cd.off; wordIdx < (cd.off + cd.len); ++wordIdx)
				{
					const Word& word = (*pWords1)[wordIdx];

					if (word.type == charType::ALPHANUMCHAR)
						++matchingWordsCount;
				}
			}
		}

		step = (step / 2) + (step % 2);

		if (bestMatchWordsCount > matchingWordsCount)
		{
			line -= step;
		}
		else
		{
			// No match at all
			if (matchingWordsCount == 0)
				return 0;

			bestMatchWordsCount = matchingWordsCount;
			bestMatchingSubChunkOffset = line;
			line += step;
		}
	}

	return bestMatchingSubChunkOffset;
}


void compareLines(diff_info& blockDiff1, diff_info& blockDiff2, const chunk_info& chunk1, const chunk_info& chunk2)
{
	for (int line1 = 0; line1 < chunk1.lineCount; ++line1)
	{
		if (chunk1.lineMappings[line1] == -1)
			continue;

		const int line2 = chunk1.lineMappings[line1];

		const std::vector<Word> words1(chunk1.words.begin() + chunk1.lineStartWordIdx[line1],
				chunk1.words.begin() + chunk1.lineEndWordIdx[line1]);
		const std::vector<Word> words2(chunk2.words.begin() + chunk2.lineStartWordIdx[line2],
				chunk2.words.begin() + chunk2.lineEndWordIdx[line2]);

		diff_info* pBlockDiff1 = &blockDiff1;
		diff_info* pBlockDiff2 = &blockDiff2;

		const std::vector<Word>* pWords1 = &words1;
		const std::vector<Word>* pWords2 = &words2;

		const int* pLine1 = &line1;
		const int* pLine2 = &line2;

		if (pWords1->size() > pWords2->size())
		{
			std::swap(pBlockDiff1, pBlockDiff2);
			std::swap(pWords1, pWords2);
			std::swap(pLine1, pLine2);
		}

		// Compare the two lines
		const std::vector<diff_info> lineDiff = DiffCalc<Word>(*pWords1, *pWords2)();

		const int lineDiffSize = static_cast<int>(lineDiff.size());

		if (lineDiffSize == 0)
			continue;

		pBlockDiff1->changedLines.emplace_back(*pLine1);
		pBlockDiff2->changedLines.emplace_back(*pLine2);

		for (int i = 0; i < lineDiffSize; ++i)
		{
			const diff_info& ld = lineDiff[i];

			if (ld.type == diff_type::DIFF_IN_1)
			{
				section_t change;

				change.off = (*pWords1)[ld.off].pos;
				change.len = (*pWords1)[ld.off + ld.len - 1].pos - change.off + (*pWords1)[ld.off + ld.len - 1].length;

				pBlockDiff1->changedLines.back().changes.emplace_back(change);
			}
			else if (ld.type == diff_type::DIFF_IN_2)
			{
				section_t change;

				change.off = (*pWords2)[ld.off].pos;
				change.len = (*pWords2)[ld.off + ld.len - 1].pos - change.off + (*pWords2)[ld.off + ld.len - 1].length;

				pBlockDiff2->changedLines.back().changes.emplace_back(change);
			}
		}
	}
}


bool compareBlocks(const DocCmpInfo& doc1, const DocCmpInfo& doc2, const UserSettings& settings,
		diff_info& blockDiff1, diff_info& blockDiff2)
{
	chunk_info chunk1(blockDiff1.off, blockDiff1.len);
	chunk_info chunk2(blockDiff2.off, blockDiff2.len);

	getWords(doc1.view, settings, chunk1);
	getWords(doc2.view, settings, chunk2);

	diff_info* pBlockDiff1 = &blockDiff1;
	diff_info* pBlockDiff2 = &blockDiff2;

	chunk_info* pChunk1 = &chunk1;
	chunk_info* pChunk2 = &chunk2;

	int startLine1 = 0;
	int startLine2 = 0;

	int* pStartLine1 = &startLine1;
	int* pStartLine2 = &startLine2;

	if (pChunk1->lineCount > pChunk2->lineCount)
	{
		std::swap(pBlockDiff1, pBlockDiff2);
		std::swap(pChunk1, pChunk2);
		std::swap(pStartLine1, pStartLine2);
	}

	// We are comparing the chunks word by word. If their line counts doesn't match some words might reappear and be
	// matched at different lines which will lead to inaccurate line changes detection. Thus we are locating here a
	// sub-chunk of the longer chunk that best matches the shorter one.
	*pStartLine2 = getBestMatchingSubChunkOffset(pChunk1, pChunk2);

	const std::vector<Word>
			words2(pChunk2->words.begin() + pChunk2->lineStartWordIdx[*pStartLine2], pChunk2->words.end());

	const std::vector<Word>* pWords1 = &(pChunk1->words);
	const std::vector<Word>* pWords2 = &words2;

	if (pWords1->size() > pWords2->size())
	{
		std::swap(pWords1, pWords2);
		std::swap(pChunk1, pChunk2);
		std::swap(pBlockDiff1, pBlockDiff2);
		std::swap(pStartLine1, pStartLine2);
	}

	// Compare the two chunks
	const std::vector<diff_info> chunkDiff = DiffCalc<Word>(*pWords1, *pWords2)();

	const int chunkDiffSize = static_cast<int>(chunkDiff.size());

	if (chunkDiffSize == 0)
		return false;

	std::vector<std::vector<int>> lineWordsMatch(pChunk1->lineCount, std::vector<int>(pChunk2->lineCount, 0));
	std::vector<int> line2Len(pChunk2->lineCount, 0);

	for (int line = 0; line < pChunk2->lineCount; ++line)
	{
		for (int i = pChunk2->lineStartWordIdx[line]; i < pChunk2->lineEndWordIdx[line]; ++i)
		{
			if (pChunk2->words[i].type == charType::ALPHANUMCHAR)
				++line2Len[line];
		}
	}

	// Use the MATCH results to synchronize line numbers (count the matching words of each line)
	for (int i = 0, wordOffset = 0; i < chunkDiffSize; ++i)
	{
		const diff_info& cd = chunkDiff[i];

		if (cd.type == diff_type::DIFF_IN_1)
		{
			wordOffset -= cd.len;
		}
		else if (cd.type == diff_type::DIFF_IN_2)
		{
			wordOffset += cd.len;
		}
		else if (cd.type == diff_type::DIFF_MATCH)
		{
			for (int wordIdx = cd.off; wordIdx < (cd.off + cd.len); ++wordIdx)
			{
				const Word& word1 = pChunk1->words[wordIdx + pChunk1->lineStartWordIdx[*pStartLine1]];

				if (word1.type == charType::ALPHANUMCHAR)
				{
					const Word& word2 = pChunk2->words[wordIdx + pChunk2->lineStartWordIdx[*pStartLine2] + wordOffset];

					++lineWordsMatch[word1.line][word2.line];
				}
			}
		}
	}

	int bestMatchingLine2 = 0;

	// Select the line with the most word matches
	for (int line1 = 0; line1 < pChunk1->lineCount; ++line1)
	{
		if (pBlockDiff1->isMoved(line1))
			continue;

		int maxConvergence = 0;
		int maxLine2Len = 0;

		for (int line2 = bestMatchingLine2; line2 < pChunk2->lineCount; ++line2)
		{
			if (lineWordsMatch[line1][line2] == 0 || line2Len[line2] == 0 || pBlockDiff2->isMoved(line2))
				continue;

			const int lineConvergence = (lineWordsMatch[line1][line2] * 100) / line2Len[line2];

			if (lineConvergence > maxConvergence ||
				(lineConvergence == maxConvergence && line2Len[line2] > maxLine2Len))
			{
				maxConvergence = lineConvergence;
				maxLine2Len = line2Len[line2];
				bestMatchingLine2 = line2;
			}
		}

		// Do lines match enough to consider this a change instead of replacement?
		if (maxConvergence >= 50)
		{
			pChunk1->lineMappings[line1]				= bestMatchingLine2;
			pChunk2->lineMappings[bestMatchingLine2]	= line1;
			++bestMatchingLine2;
		}
	}

	compareLines(*pBlockDiff1, *pBlockDiff2, *pChunk1, *pChunk2);

	return true;
}


void markSection(const diff_info& bd, const DocCmpInfo& doc)
{
	const int endOff = doc.section.off + doc.section.len;

	for (int i = doc.section.off, line = bd.off + doc.section.off; i < endOff; ++i, ++line)
	{
		const int markerMask =
				(bd.isMoved(i) == NOT_MOVED) ? doc.blockDiffMask :
				(bd.isMoved(i) == MOVED) ? MARKER_MASK_MOVED : MARKER_MASK_MOVED_MULTIPLE;

		::SendMessage(doc.view, SCI_MARKERADDSET, line, markerMask);
	}
}


void markLineDiffs(HWND view1, HWND view2, const diff_info& bd, int lineIdx)
{
	int line = bd.off + bd.changedLines[lineIdx].line;
	int linePos = ::SendMessage(view1, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change : bd.changedLines[lineIdx].changes)
		markTextAsChanged(view1, linePos + change.off, change.len);

	::SendMessage(view1, SCI_MARKERADDSET, line, MARKER_MASK_CHANGED);

	line = bd.matchedDiff->off + bd.matchedDiff->changedLines[lineIdx].line;
	linePos = ::SendMessage(view2, SCI_POSITIONFROMLINE, line, 0);

	for (const auto& change : bd.matchedDiff->changedLines[lineIdx].changes)
		markTextAsChanged(view2, linePos + change.off, change.len);

	::SendMessage(view2, SCI_MARKERADDSET, line, MARKER_MASK_CHANGED);
}


bool markAllDiffs(CompareInfo& cmpInfo, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	alignmentInfo.clear();

	const int blockDiffSize = static_cast<int>(cmpInfo.diffBlocks.size());

	if (progress)
		progress->SetMaxCount(blockDiffSize);

	AlignmentPair alignPair;

	AlignmentViewData* pMainAlignData	= &alignPair.main;
	AlignmentViewData* pSubAlignData	= &alignPair.sub;

	// Make sure pMainAlignData is linked to doc1
	if (cmpInfo.doc1.view == nppData._scintillaSecondHandle)
		std::swap(pMainAlignData, pSubAlignData);

	pMainAlignData->line	= cmpInfo.doc1.section.off;
	pSubAlignData->line		= cmpInfo.doc2.section.off;

	for (int i = 0; i < blockDiffSize; ++i)
	{
		const diff_info& bd = cmpInfo.diffBlocks[i];

		if (bd.type == diff_type::DIFF_MATCH)
		{
			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= 0;

			alignmentInfo.push_back(alignPair);

			pMainAlignData->line	+= bd.len;
			pSubAlignData->line		+= bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_2)
		{
			cmpInfo.doc2.section.off = 0;
			cmpInfo.doc2.section.len = bd.len;
			markSection(bd, cmpInfo.doc2);

			pMainAlignData->diffMask	= 0;
			pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

			alignmentInfo.push_back(alignPair);

			pSubAlignData->line += bd.len;
		}
		else if (bd.type == diff_type::DIFF_IN_1)
		{
			if (bd.matchedDiff)
			{
				const int changedLinesCount = static_cast<int>(bd.changedLines.size());

				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc2.section.off = 0;

				for (int j = 0; j < changedLinesCount; ++j)
				{
					cmpInfo.doc1.section.len = bd.changedLines[j].line - cmpInfo.doc1.section.off;
					cmpInfo.doc2.section.len = bd.matchedDiff->changedLines[j].line - cmpInfo.doc2.section.off;

					if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
					{
						pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
						pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

						alignmentInfo.push_back(alignPair);
					}

					if (cmpInfo.doc1.section.len)
					{
						markSection(bd, cmpInfo.doc1);
						pMainAlignData->line += cmpInfo.doc1.section.len;
					}

					if (cmpInfo.doc2.section.len)
					{
						markSection(*bd.matchedDiff, cmpInfo.doc2);
						pSubAlignData->line += cmpInfo.doc2.section.len;
					}

					pMainAlignData->diffMask	= MARKER_MASK_CHANGED;
					pSubAlignData->diffMask		= MARKER_MASK_CHANGED;

					alignmentInfo.push_back(alignPair);

					markLineDiffs(cmpInfo.doc1.view, cmpInfo.doc2.view, bd, j);

					cmpInfo.doc1.section.off = bd.changedLines[j].line + 1;
					cmpInfo.doc2.section.off = bd.matchedDiff->changedLines[j].line + 1;

					++pMainAlignData->line;
					++pSubAlignData->line;
				}

				cmpInfo.doc1.section.len = bd.len - cmpInfo.doc1.section.off;
				cmpInfo.doc2.section.len = bd.matchedDiff->len - cmpInfo.doc2.section.off;

				if (cmpInfo.doc1.section.len || cmpInfo.doc2.section.len)
				{
					pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
					pSubAlignData->diffMask		= cmpInfo.doc2.blockDiffMask;

					alignmentInfo.push_back(alignPair);
				}

				if (cmpInfo.doc1.section.len)
				{
					markSection(bd, cmpInfo.doc1);
					pMainAlignData->line += cmpInfo.doc1.section.len;
				}

				if (cmpInfo.doc2.section.len)
				{
					markSection(*bd.matchedDiff, cmpInfo.doc2);
					pSubAlignData->line += cmpInfo.doc2.section.len;
				}

				++i;
			}
			else
			{
				cmpInfo.doc1.section.off = 0;
				cmpInfo.doc1.section.len = bd.len;
				markSection(bd, cmpInfo.doc1);

				pMainAlignData->diffMask	= cmpInfo.doc1.blockDiffMask;
				pSubAlignData->diffMask		= 0;

				alignmentInfo.push_back(alignPair);

				pMainAlignData->line += bd.len;
			}
		}

		pMainAlignData->diffMask	= 0;
		pSubAlignData->diffMask		= 0;

		alignmentInfo.push_back(alignPair);

		if (progress && !progress->Advance())
			return false;
	}

	if (progress && !progress->NextPhase())
		return false;

	return true;
}


CompareResult runCompare(const section_t& mainViewSection, const section_t& subViewSection,
		const UserSettings& settings, AlignmentInfo_t& alignmentInfo)
{
	progress_ptr& progress = ProgressDlg::Get();

	CompareInfo cmpInfo;

	cmpInfo.doc1.view		= nppData._scintillaMainHandle;
	cmpInfo.doc1.section	= mainViewSection;
	cmpInfo.doc2.view		= nppData._scintillaSecondHandle;
	cmpInfo.doc2.section	= subViewSection;

	if (settings.OldFileViewId == MAIN_VIEW)
	{
		cmpInfo.doc1.blockDiffMask = MARKER_MASK_REMOVED;
		cmpInfo.doc2.blockDiffMask = MARKER_MASK_ADDED;
	}
	else
	{
		cmpInfo.doc1.blockDiffMask = MARKER_MASK_ADDED;
		cmpInfo.doc2.blockDiffMask = MARKER_MASK_REMOVED;
	}

	if (progress)
		progress->SetMaxCount(3);

	const std::vector<unsigned int> doc1LineHashes = computeLineHashes(cmpInfo.doc1, settings);

	if (progress && !progress->Advance())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<unsigned int> doc2LineHashes = computeLineHashes(cmpInfo.doc2, settings);

	if (progress && !progress->Advance())
		return CompareResult::COMPARE_CANCELLED;

	const std::vector<unsigned int>* pLineHashes1 = &doc1LineHashes;
	const std::vector<unsigned int>* pLineHashes2 = &doc2LineHashes;

	if (pLineHashes1->size() > pLineHashes2->size())
	{
		std::swap(pLineHashes1, pLineHashes2);
		std::swap(cmpInfo.doc1, cmpInfo.doc2);
	}

	const detect_moves_type detectMoves = !settings.DetectMoves ? DONT_DETECT :
			settings.DetectMovesLineMode ? ELEMENT_BASED : BLOCK_BASED;

	cmpInfo.diffBlocks = DiffCalc<unsigned int>(*pLineHashes1, *pLineHashes2, detectMoves)();

	const int blockDiffSize = static_cast<int>(cmpInfo.diffBlocks.size());

	if (blockDiffSize == 0)
		return CompareResult::COMPARE_MATCH;

	// Currently it is impossible to set Sci annotation in the beginning of the doc so if there is a diff in the
	// beginning (alignment via annotation will probably be necessary) we insert blank line in each doc's beginning.
	// This is a workaround until it becomes possible to insert Sci annotation in the beginning of the doc.
	if ((cmpInfo.diffBlocks[0].type != diff_type::DIFF_MATCH) &&
			(!cmpInfo.doc1.section.off || !cmpInfo.doc2.section.off))
	{
		const BOOL doc1Modified = (BOOL)::SendMessage(cmpInfo.doc1.view, SCI_GETMODIFY, 0, 0);
		const BOOL doc2Modified = (BOOL)::SendMessage(cmpInfo.doc2.view, SCI_GETMODIFY, 0, 0);

		ScopedViewWriteEnabler writeEn1(cmpInfo.doc1.view);
		ScopedViewWriteEnabler writeEn2(cmpInfo.doc2.view);

		::SendMessage(cmpInfo.doc1.view, SCI_INSERTTEXT, 0,	(LPARAM)"\n");
		if (!doc1Modified)
			::SendMessage(cmpInfo.doc1.view, SCI_SETSAVEPOINT, 0, 0);

		::SendMessage(cmpInfo.doc2.view, SCI_INSERTTEXT, 0,	(LPARAM)"\n");
		if (!doc2Modified)
			::SendMessage(cmpInfo.doc2.view, SCI_SETSAVEPOINT, 0, 0);

		++cmpInfo.doc1.section.off;
		++cmpInfo.doc2.section.off;
	}

	if (cmpInfo.doc1.section.off || cmpInfo.doc2.section.off)
	{
		for (auto& bd : cmpInfo.diffBlocks)
		{
			if (bd.type == diff_type::DIFF_IN_1 || bd.type == diff_type::DIFF_MATCH)
				bd.off += cmpInfo.doc1.section.off;
			else if (bd.type == diff_type::DIFF_IN_2)
				bd.off += cmpInfo.doc2.section.off;
		}
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (progress)
		progress->SetMaxCount(blockDiffSize);

	// Do block compares
	for (int i = 0; i < blockDiffSize; ++i)
	{
		if (cmpInfo.diffBlocks[i].type == diff_type::DIFF_IN_2)
		{
			if (i > 0) // Should always be the case but check it anyway for safety
			{
				diff_info& blockDiff1 = cmpInfo.diffBlocks[i - 1];
				diff_info& blockDiff2 = cmpInfo.diffBlocks[i];

				// Check if the NEW_IN_1 / NEW_IN_2 pair includes changed lines or it's a completely replaced block
				if (blockDiff1.type == diff_type::DIFF_IN_1)
				{
					blockDiff1.matchedDiff = &blockDiff2;
					blockDiff2.matchedDiff = &blockDiff1;

					compareBlocks(cmpInfo.doc1, cmpInfo.doc2, settings, blockDiff1, blockDiff2);
				}
			}
		}

		if (progress && !progress->Advance())
			return CompareResult::COMPARE_CANCELLED;
	}

	if (progress && !progress->NextPhase())
		return CompareResult::COMPARE_CANCELLED;

	if (!markAllDiffs(cmpInfo, alignmentInfo))
		return CompareResult::COMPARE_CANCELLED;

	return CompareResult::COMPARE_MISMATCH;
}

}


CompareResult compareViews(const section_t& mainViewSection, const section_t& subViewSection,
		const UserSettings& settings, const TCHAR* progressInfo, AlignmentInfo_t& alignmentInfo)
{
	CompareResult result = CompareResult::COMPARE_ERROR;

	if (progressInfo)
		ProgressDlg::Open(progressInfo);

	try
	{
		result = runCompare(mainViewSection, subViewSection, settings, alignmentInfo);
		ProgressDlg::Close();
	}
	catch (std::exception& e)
	{
		ProgressDlg::Close();

		char msg[128];
		_snprintf_s(msg, _countof(msg), _TRUNCATE, "Exception occurred: %s", e.what());
		MessageBoxA(nppData._nppHandle, msg, "Compare Plugin", MB_OK | MB_ICONWARNING);
	}
	catch (...)
	{
		ProgressDlg::Close();

		MessageBoxA(nppData._nppHandle, "Unknown exception occurred.", "Compare Plugin", MB_OK | MB_ICONWARNING);
	}

	return result;
}
