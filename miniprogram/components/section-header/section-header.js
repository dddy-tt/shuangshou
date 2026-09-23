Component({ properties: { title: { type: String, value: '' }, copy: { type: String, value: '' }, action: { type: String, value: '' } }, methods: { tapAction() { this.triggerEvent('action'); } } });
