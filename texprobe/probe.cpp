#include <DirectXTex.h>
#include <cstdio>
#include <cstdlib>
using namespace DirectX;
int main(int argc, char** argv)
{
	if (argc < 2) return 1;
	FILE* f = nullptr; fopen_s(&f, argv[1], "rb");
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t* buf = new uint8_t[sz]; fread(buf, 1, sz, f); fclose(f);
	TexMetadata md{};
	ScratchImage si;
	HRESULT hr = LoadFromDDSMemory(buf, sz, DDS_FLAGS_NONE, &md, si);
	printf("hr=0x%08lx\n", (unsigned long)hr);
	if (FAILED(hr)) return 2;
	printf("meta %zux%zu d=%zu mips=%zu arr=%zu dim=%d cube=%d\n",
		md.width, md.height, md.depth, md.mipLevels, md.arraySize, (int)md.dimension, (int)md.IsCubemap());
	size_t n = si.GetImageCount();
	printf("nimages=%zu\n", n);
	const Image* imgs = si.GetImages();
	uint8_t* base = si.GetPixels();
	for (size_t i = 0; i < n; ++i)
		printf("[%02zu] %zux%zu rp=%zu sp=%zu off=%lld\n", i, imgs[i].width, imgs[i].height,
			imgs[i].rowPitch, imgs[i].slicePitch, (long long)(imgs[i].pixels - base));
	for (size_t m = 0; m < md.mipLevels; ++m)
	{
		const Image* p = si.GetImage(m, 0, 0);
		printf("get(%zu,0,0)=%zux%zu off=%lld\n", m, p ? p->width : 0, p ? p->height : 0,
			p ? (long long)(p->pixels - base) : -1);
	}
	return 0;
}
