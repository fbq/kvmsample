/*
 * KVM API Sample.
 * author: Xu He Jie <xuhj@cn.ibm.com>
 * author: Boqun Feng <boqun.feng@linux.vnet.ibm.com>
 */

#include <stdio.h>
#include <memory.h>
#include <sys/mman.h>
#include <pthread.h>
#include <linux/kvm.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>

#define KVM_DEVICE "/dev/kvm"
#define RAM_SIZE 512000000
#define CODE_START 0x10000
#define BINARY_FILE "test.bin"

struct kvm {
	int dev_fd;	
	int vm_fd;
	__u64 ram_size;
	__u64 ram_start;
	int kvm_version;
	struct kvm_userspace_memory_region mem;

	struct vcpu *vcpus;
	int vcpu_number;
};

struct vcpu {
	int vcpu_id;
	int vcpu_fd;
	pthread_t vcpu_thread;
	struct kvm_run *kvm_run;
	int kvm_run_mmap_size;
	struct kvm_regs regs;
	struct kvm_sregs sregs;
	void *(*vcpu_thread_func)(void *);
	struct kvm *kvm;
};

void kvm_reset_vcpu (struct vcpu *vcpu) {
	if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &(vcpu->sregs)) < 0) {
		perror("can not get sregs\n");
		exit(1);
	}

	vcpu->sregs.cs.selector = CODE_START;
	vcpu->sregs.cs.base = CODE_START * 16;
	vcpu->sregs.ss.selector = CODE_START;
	vcpu->sregs.ss.base = CODE_START * 16;
	vcpu->sregs.ds.selector = CODE_START;
	vcpu->sregs.ds.base = CODE_START *16;
	vcpu->sregs.es.selector = CODE_START;
	vcpu->sregs.es.base = CODE_START * 16;
	vcpu->sregs.fs.selector = CODE_START;
	vcpu->sregs.fs.base = CODE_START * 16;
	vcpu->sregs.gs.selector = CODE_START;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &vcpu->sregs) < 0) {
		perror("can not set sregs");
		exit(1);
	}

	vcpu->regs.rflags = 0x0000000000000002ULL;
	vcpu->regs.rip = 0;
	vcpu->regs.rsp = 0xffffffff;
	vcpu->regs.rbp= 0;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &(vcpu->regs)) < 0) {
		perror("KVM SET REGS\n");
		exit(1);
	}
}

void *kvm_cpu_thread(void *data) {
	struct vcpu *vcpu = (struct vcpu *)data;
	int ret = 0;
	kvm_reset_vcpu(vcpu);

	while (1) {
		printf("KVM start run at cpu %d\n", vcpu->vcpu_id);
		ret = ioctl(vcpu->vcpu_fd, KVM_RUN, 0);

		if (ret < 0) {
			fprintf(stderr, "KVM_RUN failed\n");
			exit(1);
		}

		switch (vcpu->kvm_run->exit_reason) {
			case KVM_EXIT_UNKNOWN:
				printf("KVM_EXIT_UNKNOWN\n");
				break;
			case KVM_EXIT_DEBUG:
				printf("KVM_EXIT_DEBUG\n");
				break;
			case KVM_EXIT_IO:
				printf("KVM_EXIT_IO\n");
				printf("cpu %d, out port: %d, data: %d\n", 
						vcpu->vcpu_id,
						vcpu->kvm_run->io.port,  
						*(int *)((char *)(vcpu->kvm_run) + vcpu->kvm_run->io.data_offset)
				      );
				sleep(1);
				break;
			case KVM_EXIT_MMIO:
				printf("KVM_EXIT_MMIO\n");
				break;
			case KVM_EXIT_INTR:
				printf("KVM_EXIT_INTR\n");
				break;
			case KVM_EXIT_SHUTDOWN:
				printf("KVM_EXIT_SHUTDOWN\n");
				goto exit_kvm;
				break;
			default:
				printf("KVM PANIC\n");
				goto exit_kvm;
		}
	}

exit_kvm:
	return 0;
}

void load_binary(struct kvm *kvm) {
	int fd = open(BINARY_FILE, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "can not open binary file\n");
		exit(1);
	}

	int ret = 0;
	char *p = (char *)kvm->ram_start;

	while(1) {
		ret = read(fd, p, 4096);
		if (ret <= 0) {
			break;
		}
		printf("read size: %d\n", ret);
		p += ret;
	}
}

struct kvm *kvm_init(void) {
	struct kvm *kvm = malloc(sizeof(struct kvm));
	kvm->dev_fd = open(KVM_DEVICE, O_RDWR);

	if (kvm->dev_fd < 0) {
		perror("open kvm device fault: ");
		return NULL;
	}

	kvm->kvm_version = ioctl(kvm->dev_fd, KVM_GET_API_VERSION, 0);

	return kvm;
}

void kvm_clean(struct kvm *kvm) {
	assert (kvm != NULL);
	close(kvm->dev_fd);
	free(kvm);
}

int kvm_create_vm(struct kvm *kvm, int ram_size) {
	int ret = 0;
	kvm->vm_fd = ioctl(kvm->dev_fd, KVM_CREATE_VM, 0);

	if (kvm->vm_fd < 0) {
		perror("can not create vm");
		return -1;
	}

	kvm->ram_size = ram_size;
	kvm->ram_start =  (__u64)mmap(NULL, kvm->ram_size, 
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 
			-1, 0);

	if ((void *)kvm->ram_start == MAP_FAILED) {
		perror("can not mmap ram");
		return -1;
	}

	kvm->mem.slot = 0;
	kvm->mem.guest_phys_addr = 0;
	kvm->mem.memory_size = kvm->ram_size;
	kvm->mem.userspace_addr = kvm->ram_start;

	ret = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &(kvm->mem));

	if (ret < 0) {
		perror("can not set user memory region");
		return ret;
	}

	return ret;
}

static void kvm_clean_vcpus(struct kvm *kvm) {
	int i;
	for (i = 0; i < kvm->vcpu_number; i++) {
		munmap(kvm->vcpus[i].kvm_run, kvm->vcpus[i].kvm_run_mmap_size);
		close(kvm->vcpus[i].vcpu_fd);
	}
}

void kvm_clean_vm(struct kvm *kvm) {
	close(kvm->vm_fd);
	munmap((void *)kvm->ram_start, kvm->ram_size);
	kvm_clean_vcpus(kvm);
}

struct vcpu *kvm_init_vcpus(struct kvm *kvm, void *(*fn)(void *)) {
	int i, j;
	assert(kvm->vcpu_number > 0);
	struct vcpu *vcpus = malloc(sizeof(struct vcpu) * kvm->vcpu_number);

	for (i = 0; i < kvm->vcpu_number; i++) {
		vcpus[i].vcpu_id = i;
		vcpus[i].vcpu_fd = ioctl(kvm->vm_fd, KVM_CREATE_VCPU, vcpus[i].vcpu_id);

		if (vcpus[i].vcpu_fd < 0) {
			perror("can not create vcpu");
			goto create_failed;
		}

		vcpus[i].kvm_run_mmap_size = ioctl(kvm->dev_fd, KVM_GET_VCPU_MMAP_SIZE, 0);

		if (vcpus[i].kvm_run_mmap_size < 0) {
			perror("can not get vcpu mmsize");
			goto kvm_run_failed;
		}

		vcpus[i].kvm_run = mmap(NULL, vcpus[i].kvm_run_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpus[i].vcpu_fd, 0);

		if (vcpus[i].kvm_run == MAP_FAILED) {
			perror("can not mmap kvm_run");
			goto kvm_run_failed;
		}

		vcpus[i].vcpu_thread_func = fn;
		vcpus[i].kvm = kvm;
	}
	return vcpus;

kvm_run_failed:
	close(vcpus[i].vcpu_fd);
create_failed:
	for (j = 0; j < i; j++) {
		munmap(vcpus[j].kvm_run, vcpus[i].kvm_run_mmap_size);
		close(vcpus[j].vcpu_fd);
	}
	perror("can not initialize all vcpus");
	free(vcpus);
	return NULL;
}

void kvm_run_vm(struct kvm *kvm) {
	int i = 0;

	for (i = 0; i < kvm->vcpu_number; i++) {
		if (pthread_create(&(kvm->vcpus[i].vcpu_thread), (const pthread_attr_t *)NULL, kvm->vcpus[i].vcpu_thread_func, &kvm->vcpus[i]) != 0) {
			perror("can not create kvm thread");
			exit(1);
		}
	}

	while (1) {
	}
	//pthread_join(kvm->vcpus->vcpu_thread, NULL);
}

int main(int argc, char **argv) {
	int ret = 0;
	struct kvm *kvm = kvm_init();

	if (kvm == NULL) {
		fprintf(stderr, "kvm init fauilt\n");
		return -1;
	}

	if (kvm_create_vm(kvm, RAM_SIZE) < 0) {
		fprintf(stderr, "create vm fault\n");
		return -1;
	}

	load_binary(kvm);

	// only support one vcpu now
	kvm->vcpu_number = 16;
	kvm->vcpus = kvm_init_vcpus(kvm, kvm_cpu_thread);

	kvm_run_vm(kvm);

	kvm_clean_vm(kvm);
	kvm_clean(kvm);
}
