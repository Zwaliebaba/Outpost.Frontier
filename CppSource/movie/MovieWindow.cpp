// Author: Giperion.

#include "MovieWindow.h"
#include "MovieCca.h"
#include "MovieD4.h"
#include "MovieField.h"
#include "MovieGraph.h"

#include <intrin.h>

// Flag helpers match the x86 instructions used by 0x06aca920 and 0x06acb0a0.
// Carry is unchanged by INC, DEC, LEA, and MOV. Shift counts are masked to 5 bits;
// a masked count of 0 leaves the flags alone.

static unsigned int LoadDword(unsigned int Address)
{
    return *(unsigned int*)Address;
}

static void StoreDword(unsigned int Address, unsigned int Value)
{
    *(unsigned int*)Address = Value;
}

static unsigned char LoadByte(unsigned int Address)
{
    return *(unsigned char*)Address;
}

static void StoreByte(unsigned int Address, unsigned char Value)
{
    *(unsigned char*)Address = Value;
}

static void Push32(unsigned int& Esp, unsigned int Value)
{
    Esp = Esp - 4u;
    StoreDword(Esp, Value);
}

static unsigned int Pop32(unsigned int& Esp)
{
    unsigned int Value = LoadDword(Esp);
    Esp = Esp + 4u;
    return Value;
}

static void SetFlags32(unsigned int Result, unsigned int& Zero, unsigned int& SignFlag)
{
    Zero = (Result == 0u) ? 1u : 0u;
    SignFlag = Result >> 31;
}

static unsigned int Add32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned long long Sum = (unsigned long long)Left + Right;
    unsigned int Result = (unsigned int)Sum;
    long long Signed = (long long)(int)Left + (long long)(int)Right;
    Carry = (unsigned int)(Sum >> 32);
    Overflow = (Signed < -2147483647LL - 1 || Signed > 2147483647LL) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Adc32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned long long Sum = (unsigned long long)Left + Right + Carry;
    unsigned int Result = (unsigned int)Sum;
    long long Signed = (long long)(int)Left + (long long)(int)Right + (long long)Carry;
    Carry = (unsigned int)(Sum >> 32);
    Overflow = (Signed < -2147483647LL - 1 || Signed > 2147483647LL) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Sub32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned long long Need = (unsigned long long)Right;
    unsigned int Result = Left - Right;
    long long Signed = (long long)(int)Left - (long long)(int)Right;
    Carry = (Left < Need) ? 1u : 0u;
    Overflow = (Signed < -2147483647LL - 1 || Signed > 2147483647LL) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Sbb32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned long long Need = (unsigned long long)Right + Carry;
    unsigned int Result = (unsigned int)((unsigned long long)Left - Need);
    long long Signed = (long long)(int)Left - (long long)(int)Right - (long long)Carry;
    Carry = (Left < Need) ? 1u : 0u;
    Overflow = (Signed < -2147483647LL - 1 || Signed > 2147483647LL) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int And32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Result = Left & Right;
    Carry = 0u;
    Overflow = 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static void Cmp32(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    Sub32(Left, Right, Carry, Zero, SignFlag, Overflow);
}

static unsigned int Neg32(unsigned int Value, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Result = 0u - Value;
    Carry = (Value != 0u) ? 1u : 0u;
    Overflow = (Value == 0x80000000u) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Inc32(unsigned int Value, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Result = Value + 1u;
    Overflow = (Value == 0x7fffffffu) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Dec32(unsigned int Value, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Result = Value - 1u;
    Overflow = (Value == 0x80000000u) ? 1u : 0u;
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Shl32(unsigned int Value, unsigned int Count, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    Count = Count & 31u;
    if (Count == 0u)
    {
        return Value;
    }
    Carry = (Value >> (32u - Count)) & 1u;
    unsigned int Result = Value << Count;
    if (Count == 1u)
    {
        Overflow = ((Value >> 31) ^ (Value >> 30)) & 1u;
    }
    else
    {
        Overflow = 0u;
    }
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static unsigned int Shr32(unsigned int Value, unsigned int Count, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    Count = Count & 31u;
    if (Count == 0u)
    {
        return Value;
    }
    Carry = (Value >> (Count - 1u)) & 1u;
    unsigned int Result = Value >> Count;
    if (Count == 1u)
    {
        Overflow = Value >> 31;
    }
    else
    {
        Overflow = 0u;
    }
    SetFlags32(Result, Zero, SignFlag);
    return Result;
}

static void CmpByte(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Diff = (Left - Right) & 0xffu;
    int Signed = (int)(signed char)Left - (int)(signed char)Right;
    Carry = (Left < Right) ? 1u : 0u;
    Zero = (Diff == 0u) ? 1u : 0u;
    SignFlag = (Diff >> 7) & 1u;
    Overflow = (Signed < -128 || Signed > 127) ? 1u : 0u;
}

static unsigned int XorByte(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Result = (Left ^ Right) & 0xffu;
    Carry = 0u;
    Overflow = 0u;
    Zero = (Result == 0u) ? 1u : 0u;
    SignFlag = (Result >> 7) & 1u;
    return Result;
}

static unsigned int SbbByte(unsigned int Left, unsigned int Right, unsigned int& Carry, unsigned int& Zero, unsigned int& SignFlag, unsigned int& Overflow)
{
    unsigned int Need = Right + Carry;
    unsigned int Result = (Left - Need) & 0xffu;
    int Signed = (int)(signed char)Left - (int)(signed char)Right - (int)Carry;
    Carry = (Left < Need) ? 1u : 0u;
    Zero = (Result == 0u) ? 1u : 0u;
    SignFlag = (Result >> 7) & 1u;
    Overflow = (Signed < -128 || Signed > 127) ? 1u : 0u;
    return Result;
}

// . Index is Dispatch[2]. Empty slots are the baseline null dwords.
// Slots 1..4, 6, 8 and 12 are Multiply*LimbPairs.
typedef void (__stdcall *FWidthProduct)(unsigned int* Left, unsigned int* Right, unsigned int* Product, unsigned int Scale);

static FWidthProduct const GWidthProducts[13] =
{
    0,
    MultiplyOneLimbPairs,
    MultiplyTwoLimbPairs,
    MultiplyThreeLimbPairs,
    MultiplyFourLimbPairs,
    0,
    MultiplySixLimbPairs,
    0,
    MultiplyEightLimbPairs,
    0,
    0,
    0,
    MultiplyTwelveLimbPairs
};

// . 3^0 through 3^15. The dword before it (0x06ada7fc) is not read
// by the loops below: they start at index Halvings-1 and walk down to index 0.
static unsigned int const GPowersOfThree[16] =
{
    0x00000001u, 0x00000003u, 0x00000009u, 0x0000001bu,
    0x00000051u, 0x000000f3u, 0x000002d9u, 0x0000088bu,
    0x000019a1u, 0x00004ce3u, 0x0000e6a9u, 0x0002b3fbu,
    0x00081bf1u, 0x001857d3u, 0x0048fb79u, 0x00daf26bu
};

// . RET 0x14. Same arity as AddLimbs. Chosen when the mark bytes match.
long __stdcall Movie_SubtractLimbsWide(unsigned int* Left, int LeftCount, unsigned int* Right, int RightCount, int Sum);

// .
long __stdcall AddLimbs(unsigned int* Left, int LeftCount, unsigned int* Right, int RightCount, int Sum);

static void CopyOrZeroBytes(unsigned int* Dest, unsigned int* Source, unsigned int ByteCount, int FillZero)
{
    unsigned int Words = ByteCount >> 2;
    unsigned int Tail = ByteCount & 3u;
    unsigned int Index;
    unsigned char* DestBytes = (unsigned char*)(Dest + Words);
    unsigned int TailIndex;
    if (FillZero == 0)
    {
        unsigned char* SourceBytes = (unsigned char*)(Source + Words);
        for (Index = 0u; Index < Words; Index = Index + 1u)
        {
            Dest[Index] = Source[Index];
        }
        for (TailIndex = 0u; TailIndex < Tail; TailIndex = TailIndex + 1u)
        {
            DestBytes[TailIndex] = SourceBytes[TailIndex];
        }
    }
    else
    {
        for (Index = 0u; Index < Words; Index = Index + 1u)
        {
            Dest[Index] = 0u;
        }
        for (TailIndex = 0u; TailIndex < Tail; TailIndex = TailIndex + 1u)
        {
            DestBytes[TailIndex] = 0;
        }
    }
}

// . Exponent is one limb. Below 2, Dest is replaced by the field one
// (Context+0x451) or by Base, and the return is 0. Otherwise the bits below the
// top pair walk a 2-bit window. The return is squares plus multiplies, including
// when Dest is null and the limb calls are skipped.
int __stdcall Movie_ExponentiateLimb(unsigned int* Base, unsigned int Exponent, unsigned int* Dest, unsigned int* Context)
{
    unsigned int MultiplyCount = 0u;
    unsigned int SquareCount = 0u;
    unsigned int ByteCount;
    unsigned int* Source;
    if (Exponent <= 1u)
    {
        if (Dest == 0)
        {
            return 0;
        }
        Source = Context + 0x451;
        if (Exponent != 0u)
        {
            Source = Base;
        }
        ByteCount = Context[0x40] << 2;
        CopyOrZeroBytes(Dest, Source, ByteCount, 0);
        return 0;
    }

    unsigned long BitIndex = 0;
    _BitScanReverse(&BitIndex, Exponent);
    unsigned int Shift = BitIndex + 1u - 2u;
    unsigned int Top = Exponent >> Shift;
    unsigned int TopIsThree = (Top == 3u) ? 1u : 0u;
    unsigned int* Work = (unsigned int*)Movie_AllocateBlock(0x2e94u);
    if (Work == 0)
    {
        if (Dest != 0)
        {
            ByteCount = Context[0x40] << 2;
            CopyOrZeroBytes(Dest, 0, ByteCount, 1);
        }
        return 0;
    }

    unsigned char* Raw = (unsigned char*)Work;
    unsigned int* Low = Work;
    unsigned int* LowTail = (unsigned int*)(Raw + 0x514);
    unsigned int* Second = (unsigned int*)(Raw + 0x53c);
    unsigned int* SecondTail = (unsigned int*)(Raw + 0xa50);
    unsigned int* Temp = (unsigned int*)(Raw + 0xa78);
    unsigned int* Scratch = (unsigned int*)(Raw + 0xb78);
    unsigned int* Dispatch = (unsigned int*)Context[0x42];
    unsigned int LimbCount = Context[0x40];
    if (Dest != 0)
    {
        Movie_EvaluateWindow(Base, LimbCount, Low, (unsigned char*)LowTail, Dispatch);
        Movie_MultiplyTermsModulo(Low, LowTail, Low, LowTail, Dest, Context, Scratch);
    }
    SquareCount = 1u;
    if (TopIsThree != 0u)
    {
        if (Dest != 0)
        {
            Movie_MultiplyFactorModulo(Dest, Low, LowTail, Dest, Context, Scratch);
        }
        MultiplyCount = 1u;
    }
    if (Shift != 0u)
    {
        unsigned int Window = 0u;
        if (Dest != 0)
        {
            Movie_EvaluateWindow(Dest, LimbCount, Second, (unsigned char*)SecondTail, Dispatch);
        }
        while (Shift != 0u)
        {
            Shift = Shift - 1u;
            unsigned int Bit = (Exponent >> Shift) & 1u;
            Window = Bit + Window * 2u;
            // . Above 3 the switch is skipped and the shift is tested again.
            if (Window > 3u)
            {
            }
            else if (Window <= 1u)
            {
                if (Dest != 0)
                {
                    Movie_MultiplyModulo(Dest, Dest, Dest, Context, Scratch);
                }
                SquareCount = SquareCount + 1u;
            }
            else if (Window == 2u)
            {
                MultiplyCount = MultiplyCount + 1u;
                SquareCount = SquareCount + 1u;
                Window = 0u;
                if (Dest != 0)
                {
                    Movie_MultiplyFactorModulo(Dest, Low, LowTail, Dest, Context, Scratch);
                    Movie_MultiplyModulo(Dest, Dest, Dest, Context, Scratch);
                }
            }
            else
            {
                if (TopIsThree == 0u)
                {
                    if (Dest != 0)
                    {
                        Movie_MultiplyTermsModulo(Second, SecondTail, Low, LowTail, Temp, Context, Scratch);
                        Movie_EvaluateWindow(Temp, LimbCount, Second, (unsigned char*)SecondTail, Dispatch);
                    }
                    TopIsThree = 1u;
                    MultiplyCount = MultiplyCount + 1u;
                }
                if (Dest != 0)
                {
                    Movie_MultiplyModulo(Dest, Dest, Dest, Context, Scratch);
                    Movie_MultiplyFactorModulo(Dest, Second, SecondTail, Dest, Context, Scratch);
                }
                MultiplyCount = MultiplyCount + 1u;
                SquareCount = SquareCount + 1u;
                Window = 0u;
            }
        }
        if (Window != 0u)
        {
            if (Dest != 0)
            {
                Movie_MultiplyFactorModulo(Dest, Low, LowTail, Dest, Context, Scratch);
            }
            MultiplyCount = MultiplyCount + 1u;
        }
    }
    Movie_FreeBlock(Work);
    return (int)(MultiplyCount + SquareCount);
}

// .
void __stdcall Movie_FillByteBuffer(unsigned char* Buffer, unsigned int ByteCount)
{
    GenerateRngBytes(0, 0, Buffer, ByteCount);
}

// . Dispatch[0] is the width, [1] the halving count, [2] the width-product
// index. The first call uses 3^Halvings. Each later step folds Product with one
// power, walking Dispatch[2] upward and the power table downward.
void __stdcall Movie_MultiplyWindows(unsigned int* Left, unsigned int* LeftTail, unsigned int* Right, unsigned int* RightTail, unsigned int* Product, unsigned int* Dispatch)
{
    unsigned int Halvings = Dispatch[1];
    unsigned int Width = Dispatch[0];
    unsigned int Selector = Dispatch[2];
    GWidthProducts[Selector](Left, Right, Product, GPowersOfThree[Halvings]);
    if (Halvings == 0u)
    {
        return;
    }
    unsigned int Shift = Halvings;
    unsigned int* Factor = Dispatch + 2;
    unsigned int const* Power = GPowersOfThree + (Halvings - 1u);
    while (Shift != 0u)
    {
        unsigned int Scale = *Power;
        unsigned int Half = Scale >> 1;
        Movie_FoldPartialProducts(
            Product,
            Width >> (Shift & 31u),
            Scale,
            (*Factor) << 1,
            (unsigned char*)LeftTail + Half,
            (unsigned char*)RightTail + Half);
        Power = Power - 1;
        Factor = Factor + 1;
        Shift = Shift - 1u;
    }
}

// . Halvings == 0 calls the width product with scale 1 and returns.
// Otherwise the slot is Dispatch[Halvings+1]. Mark bytes choose add versus subtract
// at the end: equal selects 0x06ac8910, different selects 0x06ac8500.
void __stdcall Movie_MultiplyWindowCross(unsigned int* Left, unsigned char* LeftMark, unsigned int* Right, unsigned char* RightMark, unsigned int* Middle, unsigned int* Product, unsigned int* Dispatch)
{
    unsigned int Selector = Dispatch[2];
    unsigned int Halvings = Dispatch[1];
    FWidthProduct ProductFn = GWidthProducts[Selector];
    if (Halvings == 0u)
    {
        ProductFn(Left, Right, Product, 1u);
        return;
    }

    unsigned int Slot = Dispatch[Halvings + 1u];
    unsigned int Width = Dispatch[0];
    unsigned int Mask = (LeftMark[0] == RightMark[0]) ? 0u : 0xffffffffu;
    ProductFn(Left + Slot, Right + Slot, Product + (Slot * 2u), GPowersOfThree[Halvings - 1u] << 1);
    if (Halvings != 1u)
    {
        unsigned int Shift = Halvings;
        unsigned int LeftTrips = Halvings - 1u;
        unsigned int const* Power = GPowersOfThree + (Halvings - 2u);
        unsigned int* Factor = Dispatch + 2;
        while (LeftTrips != 0u)
        {
            unsigned int Current = Power[0];
            unsigned int Next = Power[1];
            unsigned int Sum = (Next >> 1) + Current;
            Movie_FoldPartialProducts(
                Product + (Slot * 2u),
                Width >> (Shift & 31u),
                Current * 2u,
                (*Factor) << 1,
                LeftMark + Sum,
                RightMark + Sum);
            Power = Power - 1;
            Factor = Factor + 1;
            Shift = Shift - 1u;
            LeftTrips = LeftTrips - 1u;
        }
    }

    unsigned int Half = Width >> 1;
    unsigned int CarryOut = 0u;
    unsigned int Adjust = 2u - Mask;
    unsigned int Index;
    if (Half != 0u)
    {
        for (Index = 0u; Index < Half; Index = Index + 1u)
        {
            unsigned int FromMiddle = Middle[Index];
            unsigned int FromSlot16 = Product[(Slot * 4u) + Index];
            unsigned int FromSlot8 = Product[(Slot * 2u) + Index];
            unsigned int FromHalf = Product[Half + Index];
            unsigned int Carry = 0u;
            unsigned int Zero = 0u;
            unsigned int SignFlag = 0u;
            unsigned int Overflow = 0u;
            unsigned int Mixed = FromSlot16 ^ Mask;
            unsigned int High = 0u;
            Mixed = Add32(Mixed, FromHalf, Carry, Zero, SignFlag, Overflow);
            High = Adc32(High, 0u, Carry, Zero, SignFlag, Overflow);
            Mixed = Add32(Mixed, ~FromSlot8, Carry, Zero, SignFlag, Overflow);
            High = Adc32(High, 0u, Carry, Zero, SignFlag, Overflow);
            Mixed = Add32(Mixed, ~FromMiddle, Carry, Zero, SignFlag, Overflow);
            High = Adc32(High, 0u, Carry, Zero, SignFlag, Overflow);
            Mixed = Add32(Mixed, Adjust, Carry, Zero, SignFlag, Overflow);
            High = Adc32(High, 0u, Carry, Zero, SignFlag, Overflow);
            Adjust = High;
            Mixed = Mixed + CarryOut;
            unsigned int Sum = FromSlot8 + Mixed;
            Product[Index] = FromMiddle;
            Product[Half + Index] = FromMiddle;
            Product[(Half * 2u) + Index] = Sum;
            if (Mixed != 0u)
            {
                CarryOut = (Sum < Mixed) ? 1u : 0u;
            }
        }
    }

    unsigned int* AtHalf = Product + Half;
    unsigned int* AtTwice = Product + (Half * 2u);
    unsigned int* AtTriple = Product + (Half * 3u);
    unsigned int* AtPair = Product + (Half + Slot * 2u);
    if (Half != 0u)
    {
        if (AtPair == AtTriple)
        {
            unsigned int Limb = AtTriple[0];
            unsigned int Limit = 0xffffffffu - CarryOut;
            if (Limb > Limit)
            {
                AddLimbs(AtPair, (int)Half, &CarryOut, 1, (int)AtTriple);
            }
            else
            {
                AtTriple[0] = Limb + CarryOut;
            }
        }
        else
        {
            AddLimbs(AtPair, (int)Half, &CarryOut, 1, (int)AtTriple);
        }
    }
    AddLimbs(AtHalf, (int)(Half * 3u), AtTwice, (int)(Half * 2u), (int)AtHalf);
    if (Mask == 0u)
    {
        Movie_SubtractLimbsWide(AtHalf, (int)(Half * 3u), Product + (Slot * 4u), (int)(Half * 2u), (int)AtHalf);
    }
    else
    {
        AddLimbs(AtHalf, (int)(Half * 3u), Product + (Slot * 4u), (int)(Half * 2u), (int)AtHalf);
    }
}

// . Count above Dispatch[0] sets status 0x37 and returns.
// Otherwise the source is copied, the gap to the width is zeroed, and each
// halving calls 0x06acb0a0. DestTail is a byte address: the sign base adds Power>>1.
void __stdcall Movie_EvaluateWindow(unsigned int* Source, unsigned int Count, unsigned int* Dest, unsigned char* DestTail, unsigned int* Dispatch)
{
    unsigned int Depth = Dispatch[1];
    unsigned int Width = Dispatch[0];
    unsigned int WordIndex = Dispatch[Depth + 2u];
    if (Count > Width)
    {
        GMovieGraphCode = 0x37;
        return;
    }

    unsigned int CopyBytes = Count << 2;
    CopyOrZeroBytes(Dest, Source, CopyBytes, 0);
    unsigned int Gap = Width - Count;
    unsigned int GapBytes = Gap << 2;
    CopyOrZeroBytes(Dest + Count, 0, GapBytes, 1);
    if (Depth == 0u)
    {
        return;
    }

    unsigned int Level = 0u;
    unsigned int* Factor = Dispatch + Depth + 1u;
    while (Level != Depth)
    {
        unsigned int Power = GPowersOfThree[Level];
        unsigned int Scale = *Factor;
        Movie_SubtractHalves(
            Dest + WordIndex,
            Width >> ((Level + 1u) & 31u),
            Power,
            Scale,
            DestTail + (Power >> 1));
        Level = Level + 1u;
        Factor = Factor - 1;
    }
}

// . Each statement is one listing instruction. Labels are the
// listing addresses. Groups == 0 returns before the limb work.
void __stdcall Movie_FoldPartialProducts(unsigned int* Base, unsigned int Count, unsigned int Groups, unsigned int Step, unsigned char* SignLeft, unsigned char* SignRight)
{
    unsigned int SpaceWords[40];
    unsigned int Esp = (unsigned int)(SpaceWords + 24);
    StoreDword(Esp + 4u, (unsigned int)Base);
    StoreDword(Esp + 8u, Count);
    StoreDword(Esp + 12u, Groups);
    StoreDword(Esp + 16u, Step);
    StoreDword(Esp + 20u, (unsigned int)SignLeft);
    StoreDword(Esp + 24u, (unsigned int)SignRight);
    unsigned int Eax = 0u;
    unsigned int Ecx = 0u;
    unsigned int Edx = 0u;
    unsigned int Ebx = 0u;
    unsigned int Esi = 0u;
    unsigned int Edi = 0u;
    unsigned int Ebp = 0u;
    unsigned int Carry = 0u;
    unsigned int Zero = 0u;
    unsigned int SignFlag = 0u;
    unsigned int Overflow = 0u;
    Eax = LoadDword((Esp) + 12u);
    Esp = Sub32(Esp, 40u, Carry, Zero, SignFlag, Overflow);
    Eax = Neg32(Eax, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esp) + 24u, Ebp);
    if (Zero != 0u) { goto FinishWideWindow; }
    StoreDword((Esp) + 28u, Ebx);
    StoreDword((Esp) + 32u, Edi);
    StoreDword((Esp) + 36u, Esi);
    Ebp = Eax;
    Ecx = LoadDword((Esp) + 64u);
    Ebx = LoadDword((Esp) + 60u);
    Edi = LoadDword((Esp) + 56u);
    Ecx = Sub32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esp) + 48u);
    Esi = LoadDword((Esp) + 44u);
    Ebx = Sub32(Ebx, Ebp, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esp) + 12u, Ecx);
    Ecx = Edi + Edi * 2u;
    Edi = Sub32(Edi, Edx, Carry, Zero, SignFlag, Overflow);
    Esi = Esi + Edx * 4u;
    Cmp32(Edi, Edx, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esp) + 8u, Ebx);
    if (Zero == 0u) { goto ScaleFirstHalf; }
    Cmp32(Edx, 4u, Carry, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto AccumulateTrailingWindow; }
    Cmp32(Edx, 3u, Carry, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto AccumulateLeadingWindow; }
ScaleFirstHalf:
    Edx = Shr32(Edx, 1u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esp) + 20u, Ecx);
    StoreDword((Esp) + 16u, Edi);
    if (Carry == 0u) { goto PrepareSecondHalf; }
    if (Carry != 0u) { goto RestoreWideWindowState; }
LoadFirstPair:
    Ebx = LoadDword(Esi + Edi * 8u);
    Ecx = LoadDword((Esi + Edi * 8u) + 4u);
    Eax = Sbb32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esi) + 4u);
    StoreDword(Esi, Eax);
    Eax = LoadDword((Esi) + 8u);
    Ebx = Sbb32(Ebx, Ecx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esi + Edi * 8u) + 8u);
    StoreDword((Esi) + 4u, Ebx);
    Ebx = LoadDword((Esi) + 12u);
    Eax = Sbb32(Eax, Ecx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esi + Edi * 8u) + 12u);
    StoreDword((Esi) + 8u, Eax);
    Esi = (Esi) + 16u;
    Ebx = Sbb32(Ebx, Ecx, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi);
    StoreDword((Esi) - 4u, Ebx);
    if (Zero == 0u) { goto LoadFirstPair; }
    Ecx = Edx;
    Ebx = LoadDword((Esp) + 4u);
SubtractPairAndAdvance:
    Eax = Sbb32(Eax, Ebp, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword(Esi, Eax);
    if (Carry == 0u) { goto RestoreFirstHalfState; }
    Eax = LoadDword((Esi) + 4u);
    Esi = (Esi) + 4u;
    if (Zero == 0u) { goto SubtractPairAndAdvance; }
RestoreFirstHalfState:
    Ebp = LoadDword((Esp) + 4u);
    Ecx = LoadDword((Esp) + 20u);
    Esi = LoadDword(Esp);
    Ebp = Inc32(Ebp, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto RestoreWideWindowState; }
    Edi = Sub32(Edi, Edx, Carry, Zero, SignFlag, Overflow);
    Esi = Esi + Ecx * 4u;
PrepareSecondHalf:
    StoreDword((Esp) + 4u, Ebp);
    StoreDword(Esp, Esi);
    Ebp = Edx;
    Eax = LoadDword(Esi + Edi * 4u);
LoadSecondPair:
    Ebx = LoadDword(Esi);
    Ecx = LoadDword((Esi) + 4u);
    Ebx = Adc32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Esi + Edi * 4u) + 4u);
    StoreDword(Esi + Edx * 8u, Ebx);
    Esi = (Esi) + 8u;
    Ecx = Adc32(Ecx, Eax, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi + Edi * 4u);
    StoreDword((Esi + Edx * 8u) - 4u, Ecx);
    if (Zero == 0u) { goto LoadSecondPair; }
    Ebp = Edx;
    Ebx = LoadDword((Esi + Edi * 4u) + 4u);
AdjustSecondLowWord:
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword(Esp);
    Ebx = Adc32(Ebx, 0u, Carry, Zero, SignFlag, Overflow);
    /* nop */
StoreSecondPair:
    StoreDword(Esi + Edx * 8u, Eax);
    StoreDword((Esi + Edx * 8u) + 4u, Ebx);
    Eax = LoadDword((Esi + Edi * 4u) + 8u);
    Ebx = LoadDword((Esi + Edi * 4u) + 12u);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Esi = (Esi) + 8u;
    if (Carry == 0u && Zero == 0u) { goto StoreSecondPair; }
    if (Zero == 0u) { goto AdjustSecondLowWord; }
    Ebp = Sub32(Ebp, Edx, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword(Ecx + Edx * 8u);
    Edi = Edx;
    Edx = And32(Edx, Edx, Carry, Zero, SignFlag, Overflow);
AccumulateLeadingPair:
    Ebx = LoadDword(Ecx + Ebp * 8u);
    Esi = LoadDword((Ecx + Ebp * 8u) + 4u);
    Ebx = Adc32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Ecx + Edx * 8u) + 4u);
    StoreDword(Ecx, Ebx);
    Ecx = (Ecx) + 8u;
    Esi = Adc32(Esi, Eax, Carry, Zero, SignFlag, Overflow);
    Edi = Dec32(Edi, Zero, SignFlag, Overflow);
    Eax = LoadDword(Ecx + Edx * 8u);
    StoreDword((Ecx) - 4u, Esi);
    if (Zero == 0u) { goto AccumulateLeadingPair; }
    Eax = LoadDword(Ecx);
    Ebp = Edx;
AccumulateTrailingPair:
    Ebx = LoadDword(Ecx + Edx * 8u);
    Esi = LoadDword((Ecx + Edx * 8u) + 4u);
    Ebx = Adc32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Ecx) + 4u);
    StoreDword(Ecx, Ebx);
    Ecx = (Ecx) + 8u;
    Esi = Adc32(Esi, Eax, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Eax = LoadDword(Ecx);
    StoreDword((Ecx) - 4u, Esi);
    if (Zero == 0u) { goto AccumulateTrailingPair; }
    Ebp = Edx;
    Ebx = LoadDword((Esp) + 4u);
ApplyPairCarry:
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esp) + 8u);
    StoreDword(Ecx, Eax);
    if (Carry == 0u) { goto RestoreSecondHalfState; }
    Eax = LoadDword((Ecx) + 4u);
    Ecx = (Ecx) + 4u;
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyPairCarry; }
RestoreSecondHalfState:
    Ecx = LoadDword((Esp) + 12u);
    Esi = LoadDword(Esp);
    Eax = (Eax & 0xffff00ffu) | ((LoadByte(Edi + Ebx) & 0xffu) << 8);
    Edi = LoadDword((Esp) + 16u);
    Eax = (Eax & 0xffffff00u) | (LoadByte(Ecx + Ebx) & 0xffu);
    Ebp = Edx;
    Edi = Add32(Edi, Edx, Carry, Zero, SignFlag, Overflow);
    Eax = (Eax & 0xffff00ffu) | ((XorByte(((Eax >> 8) & 0xffu), (Eax & 0xffu), Carry, Zero, SignFlag, Overflow) & 0xffu) << 8);
    Eax = LoadDword(Esi);
    if (Zero != 0u) { goto LoadFirstPair; }
LoadOutputPair:
    Ebx = LoadDword(Esi + Edi * 8u);
    Ecx = LoadDword((Esi + Edi * 8u) + 4u);
    Eax = Adc32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esi) + 4u);
    StoreDword(Esi, Eax);
    Eax = LoadDword((Esi) + 8u);
    Ebx = Adc32(Ebx, Ecx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esi + Edi * 8u) + 8u);
    StoreDword((Esi) + 4u, Ebx);
    Ebx = LoadDword((Esi) + 12u);
    Eax = Adc32(Eax, Ecx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esi + Edi * 8u) + 12u);
    StoreDword((Esi) + 8u, Eax);
    Esi = (Esi) + 16u;
    Ebx = Adc32(Ebx, Ecx, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi);
    StoreDword((Esi) - 4u, Ebx);
    if (Zero == 0u) { goto LoadOutputPair; }
    Ecx = Edx;
    Ebx = LoadDword((Esp) + 4u);
AddPairAndAdvance:
    Eax = Adc32(Eax, Ebp, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword(Esi, Eax);
    if (Carry == 0u) { goto RestoreFirstHalfState; }
    Eax = LoadDword((Esi) + 4u);
    Esi = (Esi) + 4u;
    if (Zero == 0u) { goto AddPairAndAdvance; }
    if (Zero != 0u) { goto RestoreFirstHalfState; }
ApplyLeadingWindowEdge:
    Ebx = Sub32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 32u);
    StoreDword((Esi) - 72u, Ebx);
    Ebx = LoadDword((Esi) - 64u);
    Ecx = Sbb32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 28u);
    StoreDword((Esi) - 68u, Ecx);
    Ecx = LoadDword((Esi) - 60u);
    Ebx = Sbb32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 24u);
    StoreDword((Esi) - 64u, Ebx);
    Ebx = LoadDword((Esi) - 56u);
    Ecx = Sbb32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 20u);
    StoreDword((Esi) - 60u, Ecx);
    Ecx = LoadDword((Esi) - 52u);
    Ebx = Sbb32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 16u);
    Ecx = Sbb32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 48u);
    Edi = Sbb32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 56u, Ebx);
    Edx = Sbb32(Edx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 52u, Ecx);
    Eax = Sbb32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 48u, Edi);
    Ebp = Inc32(Ebp, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 44u, Edx);
    StoreDword((Esi) - 40u, Eax);
    if (Zero != 0u) { goto RestoreWideWindowState; }
AccumulateLeadingWindow:
    Eax = LoadDword((Esi) + 12u);
    Edx = LoadDword(Esi);
    Edi = LoadDword((Esi) + 4u);
    Ebx = LoadDword((Esi) + 16u);
    Eax = Add32(Eax, Edx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esi) + 20u);
    Ebx = Adc32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 8u);
    Ecx = Adc32(Ecx, Edx, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) + 24u);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 28u);
    Edx = Adc32(Edx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 24u, Edi);
    Edi = LoadDword((Esi) + 32u);
    StoreDword((Esi) + 28u, Edx);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) - 12u);
    Edx = Add32(Edx, Eax, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 32u, Edi);
    Edi = LoadDword((Esi) - 8u);
    StoreDword(Esi, Edx);
    Edi = Adc32(Edi, Ebx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) - 4u);
    Edx = Adc32(Edx, Ecx, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 4u, Edi);
    Edi = LoadDword((Esi) + 24u);
    StoreDword((Esi) + 8u, Edx);
    Eax = Adc32(Eax, Edi, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 28u);
    Ebx = Adc32(Ebx, Edx, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 12u, Eax);
    Eax = LoadDword((Esi) + 32u);
    StoreDword((Esi) + 16u, Ebx);
    Ecx = Adc32(Ecx, Eax, Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esp) + 8u);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 20u, Ecx);
    Edx = Adc32(Edx, 0u, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esp) + 12u);
    Esi = (Esi) + 72u;
    Ebx = (Ebx & 0xffff00ffu) | ((LoadByte(Ebx + Ebp) & 0xffu) << 8);
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    Ebx = (Ebx & 0xffffff00u) | (LoadByte(Ecx + Ebp) & 0xffu);
    StoreDword((Esi) - 48u, Edi);
    CmpByte(((Ebx >> 8) & 0xffu), (Ebx & 0xffu), Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esi) - 72u);
    Ecx = LoadDword((Esi) - 68u);
    Edi = LoadDword((Esi) - 36u);
    if (Zero != 0u) { goto ApplyLeadingWindowEdge; }
    Ebx = Add32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 32u);
    StoreDword((Esi) - 72u, Ebx);
    Ebx = LoadDword((Esi) - 64u);
    Ecx = Adc32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 28u);
    StoreDword((Esi) - 68u, Ecx);
    Ecx = LoadDword((Esi) - 60u);
    Ebx = Adc32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 24u);
    StoreDword((Esi) - 64u, Ebx);
    Ebx = LoadDword((Esi) - 56u);
    Ecx = Adc32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 20u);
    StoreDword((Esi) - 60u, Ecx);
    Ecx = LoadDword((Esi) - 52u);
    Ebx = Adc32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 16u);
    Ecx = Adc32(Ecx, Edi, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 48u);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 56u, Ebx);
    Edx = Adc32(Edx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 52u, Ecx);
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 48u, Edi);
    Ebp = Inc32(Ebp, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 44u, Edx);
    StoreDword((Esi) - 40u, Eax);
    if (Zero == 0u) { goto AccumulateLeadingWindow; }
    if (Zero != 0u) { goto RestoreWideWindowState; }
ApplyMiddleWindowEdge:
    Ecx = Sub32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 44u);
    StoreDword((Esi) - 96u, Ecx);
    Ecx = LoadDword((Esi) - 88u);
    Edx = Sbb32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 40u);
    StoreDword((Esi) - 92u, Edx);
    Edx = LoadDword((Esi) - 84u);
    Ecx = Sbb32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 36u);
    StoreDword((Esi) - 88u, Ecx);
    Ecx = LoadDword((Esi) - 80u);
    Edx = Sbb32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 32u);
    StoreDword((Esi) - 84u, Edx);
    Edx = LoadDword((Esi) - 76u);
    Ecx = Sbb32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 28u);
    StoreDword((Esi) - 80u, Ecx);
    Ecx = LoadDword((Esi) - 72u);
    Edx = Sbb32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 24u);
    StoreDword((Esi) - 76u, Edx);
    Edx = LoadDword((Esi) - 68u);
    Ecx = Sbb32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 20u);
    StoreDword((Esi) - 72u, Ecx);
    Ecx = LoadDword((Esi) - 64u);
    Edx = Sbb32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esp) + 4u);
    Ecx = Sbb32(Ecx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 68u, Edx);
    Edi = Sbb32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 64u, Ecx);
    Eax = Sbb32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 60u, Edi);
    Ebx = Sbb32(Ebx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 56u, Eax);
    Ebp = Inc32(Ebp, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 52u, Ebx);
    if (Zero != 0u) { goto RestoreWideWindowState; }
AccumulateTrailingWindow:
    Eax = LoadDword((Esi) + 16u);
    Ebx = LoadDword((Esi) + 20u);
    Edx = LoadDword(Esi);
    Edi = LoadDword((Esi) + 4u);
    Eax = Add32(Eax, Edx, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esp) + 4u, Ebp);
    Ebx = Adc32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) + 8u);
    Ecx = LoadDword((Esi) + 24u);
    Edx = LoadDword((Esi) + 28u);
    Ecx = Adc32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) + 12u);
    Edx = Adc32(Edx, Edi, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) + 32u);
    Ebp = Adc32(Ebp, 0u, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) + 36u);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 32u, Ebp);
    Ebp = LoadDword((Esi) + 40u);
    StoreDword((Esi) + 36u, Edi);
    Ebp = Adc32(Ebp, 0u, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) + 44u);
    StoreDword((Esi) + 40u, Ebp);
    Ebp = LoadDword((Esi) - 16u);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    Ebp = Add32(Ebp, Eax, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 44u, Edi);
    Edi = LoadDword((Esi) - 12u);
    Edi = Adc32(Edi, Ebx, Carry, Zero, SignFlag, Overflow);
    StoreDword(Esi, Ebp);
    Ebp = LoadDword((Esi) - 8u);
    StoreDword((Esi) + 4u, Edi);
    Ebp = Adc32(Ebp, Ecx, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) - 4u);
    Edi = Adc32(Edi, Edx, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 8u, Ebp);
    Ebp = LoadDword((Esi) + 32u);
    StoreDword((Esi) + 12u, Edi);
    Eax = Adc32(Eax, Ebp, Carry, Zero, SignFlag, Overflow);
    Edi = LoadDword((Esi) + 36u);
    Ebx = Adc32(Ebx, Edi, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 16u, Eax);
    Eax = LoadDword((Esi) + 40u);
    StoreDword((Esi) + 20u, Ebx);
    Ecx = Adc32(Ecx, Eax, Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esi) + 44u);
    StoreDword((Esi) + 24u, Ecx);
    Esi = (Esi) + 96u;
    Edx = Adc32(Edx, Ebx, Carry, Zero, SignFlag, Overflow);
    Ecx = LoadDword((Esp) + 4u);
    Ebp = Adc32(Ebp, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 68u, Edx);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esp) + 8u);
    StoreDword((Esi) - 64u, Ebp);
    Ebp = LoadDword((Esp) + 12u);
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    Edx = (Edx & 0xffff00ffu) | ((LoadByte(Edx + Ecx) & 0xffu) << 8);
    Ebx = Adc32(Ebx, 0u, Carry, Zero, SignFlag, Overflow);
    Edx = (Edx & 0xffffff00u) | (LoadByte(Ebp + Ecx) & 0xffu);
    CmpByte(((Edx >> 8) & 0xffu), (Edx & 0xffu), Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 48u);
    Ecx = LoadDword((Esi) - 96u);
    Edx = LoadDword((Esi) - 92u);
    if (Zero != 0u) { goto ApplyMiddleWindowEdge; }
    Ecx = Add32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 44u);
    StoreDword((Esi) - 96u, Ecx);
    Ecx = LoadDword((Esi) - 88u);
    Edx = Adc32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 40u);
    StoreDword((Esi) - 92u, Edx);
    Edx = LoadDword((Esi) - 84u);
    Ecx = Adc32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 36u);
    StoreDword((Esi) - 88u, Ecx);
    Ecx = LoadDword((Esi) - 80u);
    Edx = Adc32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 32u);
    StoreDword((Esi) - 84u, Edx);
    Edx = LoadDword((Esi) - 76u);
    Ecx = Adc32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 28u);
    StoreDword((Esi) - 80u, Ecx);
    Ecx = LoadDword((Esi) - 72u);
    Edx = Adc32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 24u);
    StoreDword((Esi) - 76u, Edx);
    Edx = LoadDword((Esi) - 68u);
    Ecx = Adc32(Ecx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esi) - 20u);
    StoreDword((Esi) - 72u, Ecx);
    Ecx = LoadDword((Esi) - 64u);
    Edx = Adc32(Edx, Ebp, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esp) + 4u);
    Ecx = Adc32(Ecx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 68u, Edx);
    Edi = Adc32(Edi, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 64u, Ecx);
    Eax = Adc32(Eax, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 60u, Edi);
    Ebx = Adc32(Ebx, 0u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 56u, Eax);
    Ebp = Inc32(Ebp, Zero, SignFlag, Overflow);
    StoreDword((Esi) - 52u, Ebx);
    if (Zero == 0u) { goto AccumulateTrailingWindow; }
RestoreWideWindowState:
    Ebp = LoadDword((Esp) + 24u);
    Ebx = LoadDword((Esp) + 28u);
    Edi = LoadDword((Esp) + 32u);
    Esi = LoadDword((Esp) + 36u);
FinishWideWindow:
    Esp = Add32(Esp, 40u, Carry, Zero, SignFlag, Overflow);
    return;
}

// . Each statement is one listing instruction.
void __stdcall Movie_SubtractHalves(unsigned int* Limbs, unsigned int Count, unsigned int Repeats, unsigned int Step, unsigned char* Signs)
{
    unsigned int SpaceWords[40];
    unsigned int Esp = (unsigned int)(SpaceWords + 24);
    StoreDword(Esp + 4u, (unsigned int)Limbs);
    StoreDword(Esp + 8u, Count);
    StoreDword(Esp + 12u, Repeats);
    StoreDword(Esp + 16u, Step);
    StoreDword(Esp + 20u, (unsigned int)Signs);
    unsigned int Eax = 0u;
    unsigned int Ecx = 0u;
    unsigned int Edx = 0u;
    unsigned int Ebx = 0u;
    unsigned int Esi = 0u;
    unsigned int Edi = 0u;
    unsigned int Ebp = 0u;
    unsigned int Carry = 0u;
    unsigned int Zero = 0u;
    unsigned int SignFlag = 0u;
    unsigned int Overflow = 0u;
    Ecx = LoadDword((Esp) + 12u);
    Esp = Sub32(Esp, 16u, Carry, Zero, SignFlag, Overflow);
    Cmp32(Ecx, 0u, Carry, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto FinishNarrowWindow; }
    StoreDword((Esp) + 8u, Edi);
    StoreDword((Esp) + 12u, Esi);
    Edi = LoadDword((Esp) + 32u);
    Edx = LoadDword((Esp) + 24u);
    StoreDword(Esp, Ebp);
    StoreDword((Esp) + 4u, Ebx);
    Eax = Edi + Edi * 2u;
    Esi = LoadDword((Esp) + 20u);
    Eax = Shl32(Eax, 2u, Carry, Zero, SignFlag, Overflow);
    Cmp32(Edx, Edi, Carry, Zero, SignFlag, Overflow);
    Ebp = LoadDword((Esp) + 36u);
    if (Zero == 0u) { goto PrepareSampleDifference; }
    Cmp32(Edx, 4u, Carry, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto LoadTrailingWindowSamples; }
    Cmp32(Edx, 3u, Carry, Zero, SignFlag, Overflow);
    if (Zero != 0u) { goto LoadLeadingWindowSamples; }
    if (Zero == 0u) { goto PrepareSampleDifference; }
ScaleFirstSample:
    Ebp = Shr32(Ebp, 1u, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi);
LoadAdjacentSamples:
    Ebx = LoadDword(Esi + Edx * 4u);
    Ecx = LoadDword((Esi + Edx * 4u) + 4u);
    Ebx = Sbb32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Esi) + 4u);
    StoreDword(Esi + Edi * 8u, Ebx);
    Esi = (Esi) + 8u;
    Ecx = Sbb32(Ecx, Eax, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi);
    StoreDword((Esi + Edi * 8u) - 4u, Ecx);
    if (Zero == 0u) { goto LoadAdjacentSamples; }
    Ebp = Edx;
LoadPreviousSamples:
    Eax = LoadDword((Esi + Edx * 4u) - 4u);
    Ebx = LoadDword((Esi + Edx * 4u) - 8u);
    Ebp = Sub32(Ebp, 2u, Carry, Zero, SignFlag, Overflow);
    StoreDword((Esi + Edi * 4u) - 4u, Eax);
    StoreDword((Esi + Edi * 4u) - 8u, Ebx);
    Esi = (Esi) - 8u;
    if (Zero == 0u) { goto LoadPreviousSamples; }
    Ecx = Pop32(Esp);
    Ebp = Pop32(Esp);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    Eax = Pop32(Esp);
    if (Zero != 0u) { goto RestoreNarrowWindowState; }
PrepareSampleDifference:
    Ebx = (Esi + Edx * 4u) - 4u;
    Esi = Sub32(Esi, Eax, Carry, Zero, SignFlag, Overflow);
    Ebx = Sub32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Push32(Esp, Eax);
    Push32(Esp, Ebp);
    Push32(Esp, Ecx);
    Ecx = (Ebp + Ecx) - 1u;
ComputeSampleDifference:
    Eax = LoadDword(Ebx);
    Ebp = LoadDword(Ebx + Edx * 4u);
    Cmp32(Eax, Ebp, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyFirstSampleBorrow; }
    Cmp32(Esi, Ebx, Carry, Zero, SignFlag, Overflow);
    Ebx = (Ebx) - 4u;
    if (Zero == 0u) { goto ComputeSampleDifference; }
ApplyFirstSampleBorrow:
    {
    unsigned int Alu = SbbByte((Eax & 0xffu), (Eax & 0xffu), Carry, Zero, SignFlag, Overflow);
    Eax = (Eax & 0xffffff00u) | (Alu & 0xffu);
    }
    Ebp = Edx;
    StoreByte(Ecx, (unsigned char)((Eax & 0xffu)));
    if (Zero == 0u) { goto ScaleFirstSample; }
    Ebp = Shr32(Ebp, 1u, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword(Esi);
LoadNextSamples:
    Ebx = LoadDword(Esi + Edx * 4u);
    Ecx = LoadDword((Esi + Edx * 4u) + 4u);
    Eax = Sbb32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Ebx = LoadDword((Esi) + 4u);
    StoreDword(Esi + Edi * 8u, Eax);
    Esi = (Esi) + 8u;
    Ebx = Sbb32(Ebx, Ecx, Carry, Zero, SignFlag, Overflow);
    Ebp = Dec32(Ebp, Zero, SignFlag, Overflow);
    StoreDword((Esi + Edi * 8u) - 4u, Ebx);
    Eax = LoadDword(Esi);
    if (Zero == 0u) { goto LoadNextSamples; }
    Ebp = Edx;
    if (Zero != 0u) { goto LoadPreviousSamples; }
SubtractLeadingSample:
    Ebx = Sub32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 4u);
    Eax = LoadDword((Esi) + 16u);
    StoreDword((Esi) + 24u, Ebx);
    Eax = Sbb32(Eax, Edx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 8u);
    Ebx = LoadDword((Esi) + 20u);
    StoreDword((Esi) + 28u, Eax);
    Ebx = Sbb32(Ebx, Edx, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 32u, Ebx);
    if (Zero != 0u) { goto RestoreNarrowWindowState; }
LoadLeadingWindowSamples:
    Eax = LoadDword((Esi) - 28u);
    Ebx = LoadDword((Esi) - 16u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyLeadingWindowBorrow; }
    Eax = LoadDword((Esi) - 32u);
    Ebx = LoadDword((Esi) - 20u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyLeadingWindowBorrow; }
    Eax = LoadDword((Esi) - 36u);
    Ebx = LoadDword((Esi) - 24u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
ApplyLeadingWindowBorrow:
    {
    unsigned int Alu = SbbByte((Edx & 0xffu), (Edx & 0xffu), Carry, Zero, SignFlag, Overflow);
    Edx = (Edx & 0xffffff00u) | (Alu & 0xffu);
    }
    Eax = LoadDword((Esi) - 36u);
    Ebx = LoadDword((Esi) - 24u);
    Esi = (Esi) - 36u;
    StoreByte((Ebp + Ecx) - 1u, (unsigned char)((Edx & 0xffu)));
    if (SignFlag != Overflow) { goto SubtractLeadingSample; }
    Eax = Sub32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 4u);
    Ebx = LoadDword((Esi) + 16u);
    StoreDword((Esi) + 24u, Eax);
    Edx = Sbb32(Edx, Ebx, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Esi) + 8u);
    Ebx = LoadDword((Esi) + 20u);
    StoreDword((Esi) + 28u, Edx);
    Eax = Sbb32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 32u, Eax);
    if (Zero == 0u) { goto LoadLeadingWindowSamples; }
    if (Zero != 0u) { goto RestoreNarrowWindowState; }
SubtractTrailingSample:
    Ebx = Sub32(Ebx, Eax, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 4u);
    Eax = LoadDword((Esi) + 20u);
    StoreDword((Esi) + 32u, Ebx);
    Eax = Sbb32(Eax, Edx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 8u);
    Ebx = LoadDword((Esi) + 24u);
    StoreDword((Esi) + 36u, Eax);
    Ebx = Sbb32(Ebx, Edx, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Esi) + 12u);
    Edx = LoadDword((Esi) + 28u);
    StoreDword((Esi) + 40u, Ebx);
    Edx = Sbb32(Edx, Eax, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 44u, Edx);
    if (Zero != 0u) { goto RestoreNarrowWindowState; }
LoadTrailingWindowSamples:
    Eax = LoadDword((Esi) - 36u);
    Ebx = LoadDword((Esi) - 20u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyTrailingWindowBorrow; }
    Eax = LoadDword((Esi) - 40u);
    Ebx = LoadDword((Esi) - 24u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyTrailingWindowBorrow; }
    Eax = LoadDword((Esi) - 44u);
    Ebx = LoadDword((Esi) - 28u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    if (Zero == 0u) { goto ApplyTrailingWindowBorrow; }
    Eax = LoadDword((Esi) - 48u);
    Ebx = LoadDword((Esi) - 32u);
    Cmp32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
ApplyTrailingWindowBorrow:
    {
    unsigned int Alu = SbbByte((Edx & 0xffu), (Edx & 0xffu), Carry, Zero, SignFlag, Overflow);
    Edx = (Edx & 0xffffff00u) | (Alu & 0xffu);
    }
    Eax = LoadDword((Esi) - 48u);
    Ebx = LoadDword((Esi) - 32u);
    Esi = (Esi) - 48u;
    StoreByte((Ebp + Ecx) - 1u, (unsigned char)((Edx & 0xffu)));
    if (SignFlag != Overflow) { goto SubtractTrailingSample; }
    Eax = Sub32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 4u);
    Ebx = LoadDword((Esi) + 20u);
    StoreDword((Esi) + 32u, Eax);
    Edx = Sbb32(Edx, Ebx, Carry, Zero, SignFlag, Overflow);
    Eax = LoadDword((Esi) + 8u);
    Ebx = LoadDword((Esi) + 24u);
    StoreDword((Esi) + 36u, Edx);
    Eax = Sbb32(Eax, Ebx, Carry, Zero, SignFlag, Overflow);
    Edx = LoadDword((Esi) + 12u);
    Ebx = LoadDword((Esi) + 28u);
    StoreDword((Esi) + 40u, Eax);
    Edx = Sbb32(Edx, Ebx, Carry, Zero, SignFlag, Overflow);
    Ecx = Dec32(Ecx, Zero, SignFlag, Overflow);
    StoreDword((Esi) + 44u, Edx);
    if (Zero == 0u) { goto LoadTrailingWindowSamples; }
RestoreNarrowWindowState:
    Ebp = LoadDword(Esp);
    Ebx = LoadDword((Esp) + 4u);
    Edi = LoadDword((Esp) + 8u);
    Esi = LoadDword((Esp) + 12u);
FinishNarrowWindow:
    Esp = Add32(Esp, 16u, Carry, Zero, SignFlag, Overflow);
    return;
}
